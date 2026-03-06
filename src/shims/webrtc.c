#include "engine.h"
#include <rtc/rtc.h>
#include <string.h>

// PeerConnection instance storage
typedef struct {
    int rtc_id;       // libdatachannel pc id
    int js_id;        // our JS-facing id
    bool active;
} PCInstance;

// DataChannel instance storage
typedef struct {
    int rtc_id;       // libdatachannel dc id
    int js_id;        // our JS-facing id
    int pc_js_id;     // owning PC's js_id
    bool active;
} DCInstance;

#define MAX_PEER_CONNECTIONS 16
#define MAX_DATA_CHANNELS 64

static PCInstance pc_instances[MAX_PEER_CONNECTIONS];
static DCInstance dc_instances[MAX_DATA_CHANNELS];
static int next_pc_js_id = 1;
static int next_dc_js_id = 1;
static bool rtc_initialized = false;

static PCInstance *find_pc_by_rtc(int rtc_id) {
    for (int i = 0; i < MAX_PEER_CONNECTIONS; i++)
        if (pc_instances[i].active && pc_instances[i].rtc_id == rtc_id)
            return &pc_instances[i];
    return NULL;
}

static PCInstance *find_pc_by_js(int js_id) {
    for (int i = 0; i < MAX_PEER_CONNECTIONS; i++)
        if (pc_instances[i].active && pc_instances[i].js_id == js_id)
            return &pc_instances[i];
    return NULL;
}

static DCInstance *find_dc_by_rtc(int rtc_id) {
    for (int i = 0; i < MAX_DATA_CHANNELS; i++)
        if (dc_instances[i].active && dc_instances[i].rtc_id == rtc_id)
            return &dc_instances[i];
    return NULL;
}

static DCInstance *find_dc_by_js(int js_id) {
    for (int i = 0; i < MAX_DATA_CHANNELS; i++)
        if (dc_instances[i].active && dc_instances[i].js_id == js_id)
            return &dc_instances[i];
    return NULL;
}

static int alloc_dc_slot(void) {
    for (int i = 0; i < MAX_DATA_CHANNELS; i++)
        if (!dc_instances[i].active) return i;
    return -1;
}

static const char *ice_state_to_str(rtcIceState s) {
    switch (s) {
        case RTC_ICE_NEW: return "new";
        case RTC_ICE_CHECKING: return "checking";
        case RTC_ICE_CONNECTED: return "connected";
        case RTC_ICE_COMPLETED: return "completed";
        case RTC_ICE_FAILED: return "failed";
        case RTC_ICE_DISCONNECTED: return "disconnected";
        case RTC_ICE_CLOSED: return "closed";
        default: return "unknown";
    }
}

static const char *signaling_state_to_str(rtcSignalingState s) {
    switch (s) {
        case RTC_SIGNALING_STABLE: return "stable";
        case RTC_SIGNALING_HAVE_LOCAL_OFFER: return "have-local-offer";
        case RTC_SIGNALING_HAVE_REMOTE_OFFER: return "have-remote-offer";
        case RTC_SIGNALING_HAVE_LOCAL_PRANSWER: return "have-local-pranswer";
        case RTC_SIGNALING_HAVE_REMOTE_PRANSWER: return "have-remote-pranswer";
        default: return "unknown";
    }
}

// --- libdatachannel callbacks ---

static void pc_on_local_description(int pc_id, const char *sdp, const char *type, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    JSCContext *ctx = g_engine.js_ctx;

    // Store SDP in temp var to avoid escaping issues
    JSCValue *sdp_val = jsc_value_new_string(ctx, sdp);
    JSCValue *type_val = jsc_value_new_string(ctx, type);
    jsc_context_set_value(ctx, "__rtcTmpSdp", sdp_val);
    jsc_context_set_value(ctx, "__rtcTmpType", type_val);

    char js[512];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (!pc) return;"
        "  pc._localDescription = { sdp: __rtcTmpSdp, type: __rtcTmpType };"
        "  delete __rtcTmpSdp; delete __rtcTmpType;"
        // Resolve pending setLocalDescription promise
        "  if (pc._sldResolve) { pc._sldResolve(); pc._sldResolve = null; }"
        "})();", pc->js_id);
    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    if (r) g_object_unref(r);
    g_object_unref(sdp_val);
    g_object_unref(type_val);
}

static void pc_on_local_candidate(int pc_id, const char *cand, const char *mid, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    JSCContext *ctx = g_engine.js_ctx;

    JSCValue *cand_val = jsc_value_new_string(ctx, cand ? cand : "");
    JSCValue *mid_val = jsc_value_new_string(ctx, mid ? mid : "");
    jsc_context_set_value(ctx, "__rtcTmpCand", cand_val);
    jsc_context_set_value(ctx, "__rtcTmpMid", mid_val);

    char js[512];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (!pc) return;"
        "  var evt = { candidate: { candidate: __rtcTmpCand, sdpMid: __rtcTmpMid, sdpMLineIndex: 0 } };"
        "  delete __rtcTmpCand; delete __rtcTmpMid;"
        "  if (pc.onicecandidate) pc.onicecandidate(evt);"
        "})();", pc->js_id);
    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    if (r) g_object_unref(r);
    g_object_unref(cand_val);
    g_object_unref(mid_val);
}

static void pc_on_ice_state(int pc_id, rtcIceState state, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (!pc) return;"
        "  pc.iceConnectionState = '%s';"
        "  if (pc.oniceconnectionstatechange) pc.oniceconnectionstatechange({ type: 'iceconnectionstatechange' });"
        "})();", pc->js_id, ice_state_to_str(state));
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void pc_on_signaling_state(int pc_id, rtcSignalingState state, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (pc) pc.signalingState = '%s';"
        "})();", pc->js_id, signaling_state_to_str(state));
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void pc_on_state(int pc_id, rtcState state, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    const char *s = "unknown";
    switch (state) {
        case RTC_NEW: s = "new"; break;
        case RTC_CONNECTING: s = "connecting"; break;
        case RTC_CONNECTED: s = "connected"; break;
        case RTC_DISCONNECTED: s = "disconnected"; break;
        case RTC_FAILED: s = "failed"; break;
        case RTC_CLOSED: s = "closed"; break;
    }
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (!pc) return;"
        "  pc.connectionState = '%s';"
        "  if (pc.onconnectionstatechange) pc.onconnectionstatechange({ type: 'connectionstatechange' });"
        "})();", pc->js_id, s);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void pc_on_gathering_state(int pc_id, rtcGatheringState state, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;
    if (state == RTC_GATHERING_COMPLETE) {
        // Signal end of ICE candidates with null candidate
        char js[256];
        snprintf(js, sizeof(js),
            "(function(){"
            "  var pc = __pcRegistry[%d];"
            "  if (pc && pc.onicecandidate) pc.onicecandidate({ candidate: null });"
            "})();", pc->js_id);
        JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
        if (r) g_object_unref(r);
    }
}

// --- DataChannel callbacks ---

static void setup_dc_callbacks(int rtc_dc_id);

static void dc_on_open(int dc_id, void *ptr) {
    DCInstance *dc = find_dc_by_rtc(dc_id);
    if (!dc) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var dc = __dcRegistry[%d];"
        "  if (dc) { dc.readyState = 'open'; if (dc.onopen) dc.onopen({ type: 'open' }); }"
        "})();", dc->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void dc_on_closed(int dc_id, void *ptr) {
    DCInstance *dc = find_dc_by_rtc(dc_id);
    if (!dc) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var dc = __dcRegistry[%d];"
        "  if (dc) { dc.readyState = 'closed'; if (dc.onclose) dc.onclose({ type: 'close' }); }"
        "})();", dc->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
    dc->active = false;
}

static void dc_on_error(int dc_id, const char *error, void *ptr) {
    DCInstance *dc = find_dc_by_rtc(dc_id);
    if (!dc) return;
    char js[256];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var dc = __dcRegistry[%d];"
        "  if (dc && dc.onerror) dc.onerror({ type: 'error' });"
        "})();", dc->js_id);
    JSCValue *r = jsc_context_evaluate(g_engine.js_ctx, js, -1);
    if (r) g_object_unref(r);
}

static void dc_on_message(int dc_id, const char *message, int size, void *ptr) {
    DCInstance *dc = find_dc_by_rtc(dc_id);
    if (!dc) return;
    JSCContext *ctx = g_engine.js_ctx;

    if (size >= 0) {
        // Text message
        JSCValue *msg_val = jsc_value_new_string_from_bytes(ctx, g_bytes_new(message, size));
        jsc_context_set_value(ctx, "__dcMsg", msg_val);
        char js[256];
        snprintf(js, sizeof(js),
            "(function(){"
            "  var dc = __dcRegistry[%d];"
            "  if (dc && dc.onmessage) dc.onmessage({ type: 'message', data: __dcMsg });"
            "  delete __dcMsg;"
            "})();", dc->js_id);
        JSCValue *r = jsc_context_evaluate(ctx, js, -1);
        if (r) g_object_unref(r);
        g_object_unref(msg_val);
    } else {
        // Binary message
        int len = -size;
        void *copy = g_memdup2(message, len);
        JSCValue *ab = jsc_value_new_array_buffer(ctx, copy, len,
            (GDestroyNotify)g_free, copy);
        jsc_context_set_value(ctx, "__dcMsg", ab);
        char js[256];
        snprintf(js, sizeof(js),
            "(function(){"
            "  var dc = __dcRegistry[%d];"
            "  if (dc && dc.onmessage) dc.onmessage({ type: 'message', data: __dcMsg });"
            "  delete __dcMsg;"
            "})();", dc->js_id);
        JSCValue *r = jsc_context_evaluate(ctx, js, -1);
        if (r) g_object_unref(r);
        g_object_unref(ab);
    }
}

static void setup_dc_callbacks(int rtc_dc_id) {
    rtcSetOpenCallback(rtc_dc_id, dc_on_open);
    rtcSetClosedCallback(rtc_dc_id, dc_on_closed);
    rtcSetErrorCallback(rtc_dc_id, dc_on_error);
    rtcSetMessageCallback(rtc_dc_id, dc_on_message);
}

// When remote peer creates a data channel, we get notified here
static void pc_on_data_channel(int pc_id, int dc_id, void *ptr) {
    PCInstance *pc = find_pc_by_rtc(pc_id);
    if (!pc) return;

    int slot = alloc_dc_slot();
    if (slot < 0) return;

    int js_id = next_dc_js_id++;
    dc_instances[slot].rtc_id = dc_id;
    dc_instances[slot].js_id = js_id;
    dc_instances[slot].pc_js_id = pc->js_id;
    dc_instances[slot].active = true;

    setup_dc_callbacks(dc_id);

    // Get the label
    char label[256] = "";
    rtcGetDataChannelLabel(dc_id, label, sizeof(label));

    JSCContext *ctx = g_engine.js_ctx;
    JSCValue *label_val = jsc_value_new_string(ctx, label);
    jsc_context_set_value(ctx, "__dcTmpLabel", label_val);

    char js[512];
    snprintf(js, sizeof(js),
        "(function(){"
        "  var pc = __pcRegistry[%d];"
        "  if (!pc || !pc.ondatachannel) return;"
        "  var dc = __createDCObj(%d, __dcTmpLabel);"
        "  delete __dcTmpLabel;"
        "  __dcRegistry[%d] = dc;"
        "  pc.ondatachannel({ channel: dc });"
        "})();", pc->js_id, js_id, js_id);
    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    if (r) g_object_unref(r);
    g_object_unref(label_val);
}

// --- Native functions exposed to JS ---

// __pcCreate(iceServersJSON) -> js_id
static JSCValue *native_pc_create(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();

    if (!rtc_initialized) {
        rtcInitLogger(RTC_LOG_WARNING, NULL);
        rtc_initialized = true;
    }

    int slot = -1;
    for (int i = 0; i < MAX_PEER_CONNECTIONS; i++) {
        if (!pc_instances[i].active) { slot = i; break; }
    }
    if (slot < 0) return jsc_value_new_number(ctx, 0);

    // Parse ICE servers from JSON string
    rtcConfiguration config;
    memset(&config, 0, sizeof(config));

    const char *default_stun = "stun:stun.l.google.com:19302";
    config.iceServers = &default_stun;
    config.iceServersCount = 1;

    // If args provided, try to extract ICE server URLs
    // For now use default STUN server
    // PeerJS typically passes { iceServers: [{urls: "stun:..."}] }

    int rtc_id = rtcCreatePeerConnection(&config);
    if (rtc_id < 0) return jsc_value_new_number(ctx, 0);

    int js_id = next_pc_js_id++;
    pc_instances[slot].rtc_id = rtc_id;
    pc_instances[slot].js_id = js_id;
    pc_instances[slot].active = true;

    // Set all callbacks
    rtcSetLocalDescriptionCallback(rtc_id, pc_on_local_description);
    rtcSetLocalCandidateCallback(rtc_id, pc_on_local_candidate);
    rtcSetIceStateChangeCallback(rtc_id, pc_on_ice_state);
    rtcSetSignalingStateChangeCallback(rtc_id, pc_on_signaling_state);
    rtcSetStateChangeCallback(rtc_id, pc_on_state);
    rtcSetGatheringStateChangeCallback(rtc_id, pc_on_gathering_state);
    rtcSetDataChannelCallback(rtc_id, pc_on_data_channel);

    printf("[WebRTC] PeerConnection created (id=%d)\n", js_id);
    return jsc_value_new_number(ctx, js_id);
}

// __pcSetRemoteDescription(js_id, sdp, type)
static void native_pc_set_remote_desc(GPtrArray *args, gpointer user_data) {
    if (args->len < 3) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    PCInstance *pc = find_pc_by_js(js_id);
    if (!pc) return;

    char *sdp = jsc_value_to_string(g_ptr_array_index(args, 1));
    char *type = jsc_value_to_string(g_ptr_array_index(args, 2));

    rtcSetRemoteDescription(pc->rtc_id, sdp, type);

    g_free(sdp);
    g_free(type);
}

// __pcSetLocalDescription(js_id, type_or_null)
static void native_pc_set_local_desc(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    PCInstance *pc = find_pc_by_js(js_id);
    if (!pc) return;

    const char *type = NULL;
    if (args->len > 1 && jsc_value_is_string(g_ptr_array_index(args, 1))) {
        char *t = jsc_value_to_string(g_ptr_array_index(args, 1));
        rtcSetLocalDescription(pc->rtc_id, t);
        g_free(t);
    } else {
        rtcSetLocalDescription(pc->rtc_id, type);
    }
}

// __pcAddIceCandidate(js_id, candidate, sdpMid)
static void native_pc_add_ice_candidate(GPtrArray *args, gpointer user_data) {
    if (args->len < 3) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    PCInstance *pc = find_pc_by_js(js_id);
    if (!pc) return;

    char *cand = jsc_value_to_string(g_ptr_array_index(args, 1));
    char *mid = jsc_value_to_string(g_ptr_array_index(args, 2));

    rtcAddRemoteCandidate(pc->rtc_id, cand, mid);

    g_free(cand);
    g_free(mid);
}

// __pcCreateDataChannel(js_id, label, ordered) -> dc_js_id
static JSCValue *native_pc_create_data_channel(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_number(ctx, 0);

    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    PCInstance *pc = find_pc_by_js(js_id);
    if (!pc) return jsc_value_new_number(ctx, 0);

    char *label = jsc_value_to_string(g_ptr_array_index(args, 1));
    bool ordered = args->len > 2 ? jsc_value_to_boolean(g_ptr_array_index(args, 2)) : true;

    int rtc_dc_id;
    if (ordered) {
        rtc_dc_id = rtcCreateDataChannel(pc->rtc_id, label);
    } else {
        rtcDataChannelInit init;
        memset(&init, 0, sizeof(init));
        init.reliability.unordered = true;
        rtc_dc_id = rtcCreateDataChannelEx(pc->rtc_id, label, &init);
    }
    g_free(label);

    if (rtc_dc_id < 0) return jsc_value_new_number(ctx, 0);

    int slot = alloc_dc_slot();
    if (slot < 0) {
        rtcDelete(rtc_dc_id);
        return jsc_value_new_number(ctx, 0);
    }

    int dc_js_id = next_dc_js_id++;
    dc_instances[slot].rtc_id = rtc_dc_id;
    dc_instances[slot].js_id = dc_js_id;
    dc_instances[slot].pc_js_id = js_id;
    dc_instances[slot].active = true;

    setup_dc_callbacks(rtc_dc_id);

    return jsc_value_new_number(ctx, dc_js_id);
}

// __dcSend(dc_js_id, data)
static void native_dc_send(GPtrArray *args, gpointer user_data) {
    if (args->len < 2) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    DCInstance *dc = find_dc_by_js(js_id);
    if (!dc) return;

    JSCValue *data = g_ptr_array_index(args, 1);

    if (jsc_value_is_string(data)) {
        char *str = jsc_value_to_string(data);
        rtcSendMessage(dc->rtc_id, str, (int)strlen(str));
        g_free(str);
    } else if (jsc_value_is_array_buffer(data)) {
        gsize len = 0;
        void *buf = jsc_value_array_buffer_get_data(data, &len);
        if (buf && len > 0) rtcSendMessage(dc->rtc_id, buf, -(int)len);
    } else if (jsc_value_is_typed_array(data)) {
        gsize len = jsc_value_typed_array_get_size(data);
        void *buf = jsc_value_typed_array_get_data(data, NULL);
        if (buf && len > 0) rtcSendMessage(dc->rtc_id, buf, -(int)len);
    }
}

// __dcClose(dc_js_id)
static void native_dc_close(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    DCInstance *dc = find_dc_by_js(js_id);
    if (dc) rtcClose(dc->rtc_id);
}

// __pcClose(js_id)
static void native_pc_close(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    PCInstance *pc = find_pc_by_js(js_id);
    if (pc) {
        rtcClosePeerConnection(pc->rtc_id);
        rtcDeletePeerConnection(pc->rtc_id);
        pc->active = false;
    }
}

// __dcGetBufferedAmount(dc_js_id) -> int
static JSCValue *native_dc_get_buffered(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, 0);
    int js_id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    DCInstance *dc = find_dc_by_js(js_id);
    if (!dc) return jsc_value_new_number(ctx, 0);
    int amount = rtcGetBufferedAmount(dc->rtc_id);
    return jsc_value_new_number(ctx, amount >= 0 ? amount : 0);
}

void register_webrtc_shim(JSCContext *ctx) {
    memset(pc_instances, 0, sizeof(pc_instances));
    memset(dc_instances, 0, sizeof(dc_instances));

    // Register native functions
    #define REG_VAR(name, cb, ret) do { \
        JSCValue *fn = jsc_value_new_function_variadic(ctx, name, G_CALLBACK(cb), NULL, NULL, ret); \
        jsc_context_set_value(ctx, name, fn); \
        g_object_unref(fn); \
    } while(0)

    REG_VAR("__pcCreate", native_pc_create, JSC_TYPE_VALUE);
    REG_VAR("__pcSetRemoteDescription", native_pc_set_remote_desc, G_TYPE_NONE);
    REG_VAR("__pcSetLocalDescription", native_pc_set_local_desc, G_TYPE_NONE);
    REG_VAR("__pcAddIceCandidate", native_pc_add_ice_candidate, G_TYPE_NONE);
    REG_VAR("__pcCreateDataChannel", native_pc_create_data_channel, JSC_TYPE_VALUE);
    REG_VAR("__pcClose", native_pc_close, G_TYPE_NONE);
    REG_VAR("__dcSend", native_dc_send, G_TYPE_NONE);
    REG_VAR("__dcClose", native_dc_close, G_TYPE_NONE);
    REG_VAR("__dcGetBufferedAmount", native_dc_get_buffered, JSC_TYPE_VALUE);

    #undef REG_VAR

    // JavaScript RTCPeerConnection class
    jsc_context_evaluate(ctx,
        "window.__pcRegistry = {};"
        "window.__dcRegistry = {};"

        // Helper to create a DC JS object
        "window.__createDCObj = function(jsId, label) {"
        "  var dc = {"
        "    _id: jsId,"
        "    label: label,"
        "    readyState: 'connecting',"
        "    binaryType: 'arraybuffer',"
        "    ordered: true,"
        "    onopen: null,"
        "    onclose: null,"
        "    onmessage: null,"
        "    onerror: null,"
        "    _listeners: {},"
        "    get bufferedAmount() { return __dcGetBufferedAmount(this._id); },"
        "    bufferedAmountLowThreshold: 0,"
        "    send: function(data) { __dcSend(this._id, data); },"
        "    close: function() { this.readyState = 'closing'; __dcClose(this._id); },"
        "    addEventListener: function(type, cb) {"
        "      if (!this._listeners[type]) this._listeners[type] = [];"
        "      this._listeners[type].push(cb);"
        "      if (type === 'message') { var self = this; var orig = this.onmessage; this.onmessage = function(e) { if (orig) orig(e); cb(e); }; }"
        "    },"
        "    removeEventListener: function(type, cb) {"
        "      if (this._listeners[type]) {"
        "        var idx = this._listeners[type].indexOf(cb);"
        "        if (idx >= 0) this._listeners[type].splice(idx, 1);"
        "      }"
        "    }"
        "  };"
        "  return dc;"
        "};"

        // RTCPeerConnection constructor
        "window.RTCPeerConnection = function(config) {"
        "  this._id = __pcCreate();"
        "  this.signalingState = 'stable';"
        "  this.iceConnectionState = 'new';"
        "  this.connectionState = 'new';"
        "  this._localDescription = null;"
        "  this._remoteDescription = null;"
        "  this._sldResolve = null;"
        "  this.onicecandidate = null;"
        "  this.oniceconnectionstatechange = null;"
        "  this.onconnectionstatechange = null;"
        "  this.ondatachannel = null;"
        "  this.ontrack = null;"
        "  if (this._id) __pcRegistry[this._id] = this;"
        "};"

        "Object.defineProperty(RTCPeerConnection.prototype, 'localDescription', {"
        "  get: function() { return this._localDescription; }"
        "});"
        "Object.defineProperty(RTCPeerConnection.prototype, 'remoteDescription', {"
        "  get: function() { return this._remoteDescription; }"
        "});"

        "RTCPeerConnection.prototype.createOffer = function(opts) {"
        "  var self = this;"
        "  return new Promise(function(resolve) {"
        "    self._sldResolve = function() {"
        "      resolve(self._localDescription);"
        "    };"
        "    __pcSetLocalDescription(self._id, 'offer');"
        "  });"
        "};"

        "RTCPeerConnection.prototype.createAnswer = function() {"
        "  var self = this;"
        "  return new Promise(function(resolve) {"
        "    self._sldResolve = function() {"
        "      resolve(self._localDescription);"
        "    };"
        "    __pcSetLocalDescription(self._id, 'answer');"
        "  });"
        "};"

        "RTCPeerConnection.prototype.setLocalDescription = function(desc) {"
        "  var self = this;"
        "  if (desc && desc.sdp) {"
        "    return Promise.resolve();" // Already set by createOffer/createAnswer
        "  }"
        "  return new Promise(function(resolve) {"
        "    self._sldResolve = resolve;"
        "    __pcSetLocalDescription(self._id);"
        "  });"
        "};"

        "RTCPeerConnection.prototype.setRemoteDescription = function(desc) {"
        "  this._remoteDescription = desc;"
        "  var sdp = (typeof desc === 'object') ? desc.sdp : desc;"
        "  var type = (typeof desc === 'object') ? desc.type : 'offer';"
        "  __pcSetRemoteDescription(this._id, sdp, type);"
        "  return Promise.resolve();"
        "};"

        "RTCPeerConnection.prototype.addIceCandidate = function(cand) {"
        "  if (cand && cand.candidate) {"
        "    __pcAddIceCandidate(this._id, cand.candidate, cand.sdpMid || '0');"
        "  }"
        "  return Promise.resolve();"
        "};"

        "RTCPeerConnection.prototype.createDataChannel = function(label, opts) {"
        "  var ordered = opts && opts.ordered !== undefined ? opts.ordered : true;"
        "  var dcId = __pcCreateDataChannel(this._id, label, ordered);"
        "  if (!dcId) return null;"
        "  var dc = __createDCObj(dcId, label);"
        "  dc.ordered = ordered;"
        "  __dcRegistry[dcId] = dc;"
        "  return dc;"
        "};"

        "RTCPeerConnection.prototype.addTrack = function(track, stream) { return { track: track }; };"
        "RTCPeerConnection.prototype.addTransceiver = function(kind) {"
        "  return { currentDirection: null, direction: 'sendrecv', mid: null,"
        "    receiver: { track: null }, sender: { track: null } };"
        "};"

        "RTCPeerConnection.prototype.close = function() {"
        "  __pcClose(this._id);"
        "  this.signalingState = 'closed';"
        "  this.iceConnectionState = 'closed';"
        "  this.connectionState = 'closed';"
        "};"

        "RTCPeerConnection.prototype.getStats = function() { return Promise.resolve(new Map()); };"

        // RTCSessionDescription
        "window.RTCSessionDescription = function(init) {"
        "  this.sdp = init ? init.sdp : '';"
        "  this.type = init ? init.type : '';"
        "};"

        // RTCIceCandidate
        "window.RTCIceCandidate = function(init) {"
        "  this.candidate = init ? init.candidate : '';"
        "  this.sdpMid = init ? init.sdpMid : null;"
        "  this.sdpMLineIndex = init ? init.sdpMLineIndex : null;"
        "};"

        // Feature detection
        "window.RTCRtpTransceiver = function() {};"
        "RTCRtpTransceiver.prototype.currentDirection = null;"
        , -1);
}
