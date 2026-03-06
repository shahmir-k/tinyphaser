#include "engine.h"
#include <rtc/rtc.h>
#include <string.h>

// WebSocket instance storage
typedef struct {
    int rtc_id;       // libdatachannel ws id
    int js_id;        // our JS-facing id
    char *url;
    bool active;
} WSInstance;

#define MAX_WEBSOCKETS 32
static WSInstance ws_instances[MAX_WEBSOCKETS];
static int next_ws_js_id = 1;
static bool rtc_ws_initialized = false;

static WSInstance *find_ws_by_rtc(int rtc_id) {
    for (int i = 0; i < MAX_WEBSOCKETS; i++)
        if (ws_instances[i].active && ws_instances[i].rtc_id == rtc_id)
            return &ws_instances[i];
    return NULL;
}

static WSInstance *find_ws_by_js(int js_id) {
    for (int i = 0; i < MAX_WEBSOCKETS; i++)
        if (ws_instances[i].active && ws_instances[i].js_id == js_id)
            return &ws_instances[i];
    return NULL;
}

// Callbacks from libdatachannel (called from its thread pool)
// We queue JS evaluation for the main thread via a simple flag+data system
// For simplicity, we evaluate JS directly since JSC GLib is thread-aware with GMainContext

static void ws_on_open(int ws_id, void *ptr) {
    WSInstance *ws = find_ws_by_rtc(ws_id);
    if (!ws) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var ws = __wsRegistry[%d];"
        "  if (ws) { ws.readyState = 1; if (ws.onopen) ws.onopen({ type: 'open' }); }"
        "})();", ws->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void ws_on_closed(int ws_id, void *ptr) {
    WSInstance *ws = find_ws_by_rtc(ws_id);
    if (!ws) return;
    char js[512];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var ws = __wsRegistry[%d];"
        "  if (ws) {"
        "    ws.readyState = 3;"
        "    if (ws.onclose) ws.onclose({ type: 'close', code: 1000, reason: '', wasClean: true });"
        "  }"
        "})();", ws->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
    ws->active = false;
}

static void ws_on_error(int ws_id, const char *error, void *ptr) {
    WSInstance *ws = find_ws_by_rtc(ws_id);
    if (!ws) return;
    // Escape error string for JS
    char js[512];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var ws = __wsRegistry[%d];"
        "  if (ws && ws.onerror) ws.onerror({ type: 'error', message: 'WebSocket error' });"
        "})();", ws->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void ws_on_message(int ws_id, const char *message, int size, void *ptr) {
    WSInstance *ws = find_ws_by_rtc(ws_id);
    if (!ws) return;

    if (size >= 0) {
        // Text message
        // Need to escape the message for JS string embedding
        // Use a JSC approach: set a temp variable
        JSCContext *ctx = g_engine.js_ctx;
        JSCValue *msg_val = jsc_value_new_string_from_bytes(ctx, (const GBytes *)g_bytes_new(message, size));
        jsc_context_set_value(ctx, "__wsMsg", msg_val);
        char js[256];
        snprintf(js, sizeof(js),
            "(function(){"
            "  var ws = __wsRegistry[%d];"
            "  if (ws && ws.onmessage) ws.onmessage({ type: 'message', data: __wsMsg });"
            "  delete __wsMsg;"
            "})();", ws->js_id);
        JSCValue *r = jsc_context_evaluate(ctx, js, -1);
        if (r) g_object_unref(r);
        g_object_unref(msg_val);
    } else {
        // Binary message (size is negative, abs(size) is the length)
        int len = -size;
        JSCContext *ctx = g_engine.js_ctx;
        void *copy = g_memdup2(message, len);
        JSCValue *ab = jsc_value_new_array_buffer(ctx, copy, len,
            (GDestroyNotify)g_free, copy);
        jsc_context_set_value(ctx, "__wsMsg", ab);
        char js[256];
        snprintf(js, sizeof(js),
            "(function(){"
            "  var ws = __wsRegistry[%d];"
            "  if (ws && ws.onmessage) ws.onmessage({ type: 'message', data: __wsMsg });"
            "  delete __wsMsg;"
            "})();", ws->js_id);
        JSCValue *r = jsc_context_evaluate(ctx, js, -1);
        if (r) g_object_unref(r);
        g_object_unref(ab);
    }
}

// Native: __wsCreate(url) -> js_id
static JSCValue *native_ws_create(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, 0);

    char *url = jsc_value_to_string(g_ptr_array_index(args, 0));

    if (!rtc_ws_initialized) {
        rtcInitLogger(RTC_LOG_WARNING, NULL);
        rtc_ws_initialized = true;
    }

    // Find a free slot
    int slot = -1;
    for (int i = 0; i < MAX_WEBSOCKETS; i++) {
        if (!ws_instances[i].active) { slot = i; break; }
    }
    if (slot < 0) {
        fprintf(stderr, "[WebSocket] Max connections reached\n");
        g_free(url);
        return jsc_value_new_number(ctx, 0);
    }

    int rtc_id = rtcCreateWebSocket(url);
    if (rtc_id < 0) {
        fprintf(stderr, "[WebSocket] Failed to create: %s\n", url);
        g_free(url);
        return jsc_value_new_number(ctx, 0);
    }

    int js_id = next_ws_js_id++;
    ws_instances[slot].rtc_id = rtc_id;
    ws_instances[slot].js_id = js_id;
    ws_instances[slot].url = url;
    ws_instances[slot].active = true;

    rtcSetOpenCallback(rtc_id, ws_on_open);
    rtcSetClosedCallback(rtc_id, ws_on_closed);
    rtcSetErrorCallback(rtc_id, ws_on_error);
    rtcSetMessageCallback(rtc_id, ws_on_message);

    printf("[WebSocket] Created: %s (id=%d)\n", url, js_id);
    return jsc_value_new_number(ctx, js_id);
}

// Native: __wsSend(js_id, data) -> bool
static JSCValue *native_ws_send(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_boolean(ctx, FALSE);

    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    WSInstance *ws = find_ws_by_js(js_id);
    if (!ws) return jsc_value_new_boolean(ctx, FALSE);

    JSCValue *data = g_ptr_array_index(args, 1);

    if (jsc_value_is_string(data)) {
        char *str = jsc_value_to_string(data);
        int r = rtcSendMessage(ws->rtc_id, str, (int)strlen(str));
        g_free(str);
        return jsc_value_new_boolean(ctx, r >= 0);
    } else if (jsc_value_is_array_buffer(data)) {
        gsize len = 0;
        void *buf = jsc_value_array_buffer_get_data(data, &len);
        if (buf && len > 0) {
            int r = rtcSendMessage(ws->rtc_id, buf, -(int)len); // negative = binary
            return jsc_value_new_boolean(ctx, r >= 0);
        }
    } else if (jsc_value_is_typed_array(data)) {
        gsize len = jsc_value_typed_array_get_size(data);
        void *buf = jsc_value_typed_array_get_data(data, NULL);
        if (buf && len > 0) {
            int r = rtcSendMessage(ws->rtc_id, buf, -(int)len);
            return jsc_value_new_boolean(ctx, r >= 0);
        }
    }
    return jsc_value_new_boolean(ctx, FALSE);
}

// Native: __wsClose(js_id)
static void native_ws_close(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    WSInstance *ws = find_ws_by_js(js_id);
    if (ws) rtcClose(ws->rtc_id);
}

void register_websocket_shim(JSCContext *ctx) {
    memset(ws_instances, 0, sizeof(ws_instances));

    JSCValue *create_fn = jsc_value_new_function_variadic(ctx, "__wsCreate",
        G_CALLBACK(native_ws_create), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__wsCreate", create_fn);
    g_object_unref(create_fn);

    JSCValue *send_fn = jsc_value_new_function_variadic(ctx, "__wsSend",
        G_CALLBACK(native_ws_send), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__wsSend", send_fn);
    g_object_unref(send_fn);

    JSCValue *close_fn = jsc_value_new_function_variadic(ctx, "__wsClose",
        G_CALLBACK(native_ws_close), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__wsClose", close_fn);
    g_object_unref(close_fn);

    // JavaScript WebSocket class that wraps native functions
    jsc_context_evaluate(ctx,
        "window.__wsRegistry = {};"
        "window.WebSocket = function(url, protocols) {"
        "  this.url = url;"
        "  this.readyState = 0;" // CONNECTING
        "  this.binaryType = 'blob';"
        "  this.bufferedAmount = 0;"
        "  this.extensions = '';"
        "  this.protocol = '';"
        "  this.onopen = null;"
        "  this.onclose = null;"
        "  this.onmessage = null;"
        "  this.onerror = null;"
        "  this._listeners = {};"
        "  this._id = __wsCreate(url);"
        "  if (this._id) __wsRegistry[this._id] = this;"
        "};"
        "WebSocket.CONNECTING = 0;"
        "WebSocket.OPEN = 1;"
        "WebSocket.CLOSING = 2;"
        "WebSocket.CLOSED = 3;"
        "WebSocket.prototype.send = function(data) {"
        "  if (this.readyState !== 1) throw new Error('WebSocket is not open');"
        "  __wsSend(this._id, data);"
        "};"
        "WebSocket.prototype.close = function(code, reason) {"
        "  if (this.readyState >= 2) return;"
        "  this.readyState = 2;"
        "  __wsClose(this._id);"
        "};"
        "WebSocket.prototype.addEventListener = function(type, cb) {"
        "  if (!this._listeners[type]) this._listeners[type] = [];"
        "  this._listeners[type].push(cb);"
        "  if (type === 'open' && !this.onopen) this.onopen = cb;"
        "  else if (type === 'close' && !this.onclose) this.onclose = cb;"
        "  else if (type === 'message' && !this.onmessage) this.onmessage = cb;"
        "  else if (type === 'error' && !this.onerror) this.onerror = cb;"
        "};"
        "WebSocket.prototype.removeEventListener = function(type, cb) {"
        "  if (!this._listeners[type]) return;"
        "  var idx = this._listeners[type].indexOf(cb);"
        "  if (idx >= 0) this._listeners[type].splice(idx, 1);"
        "};"
        , -1);
}
