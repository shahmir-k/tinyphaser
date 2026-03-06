#include "engine.h"
#include <libsoup/soup.h>
#include <string.h>

static SoupSession *http_session = NULL;

// __httpFetch(url, method, body, headers_json) -> { status, body, headers }
// Async HTTP fetch using libsoup, returns a Promise via JS wrapper
static JSCValue *native_http_fetch(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_null(ctx);

    char *url = jsc_value_to_string(g_ptr_array_index(args, 0));
    char *method = args->len > 1 ? jsc_value_to_string(g_ptr_array_index(args, 1)) : g_strdup("GET");

    if (!http_session) {
        http_session = soup_session_new();
    }

    SoupMessage *msg = soup_message_new(method, url);
    if (!msg) {
        fprintf(stderr, "[Fetch] Invalid URL: %s\n", url);
        g_free(url);
        g_free(method);
        return jsc_value_new_null(ctx);
    }

    // Set request body if provided
    if (args->len > 2 && !jsc_value_is_null(g_ptr_array_index(args, 2)) &&
        !jsc_value_is_undefined(g_ptr_array_index(args, 2))) {
        char *body = jsc_value_to_string(g_ptr_array_index(args, 2));
        GBytes *body_bytes = g_bytes_new_take(body, strlen(body));
        soup_message_set_request_body_from_bytes(msg, "application/json", body_bytes);
        g_bytes_unref(body_bytes);
    }

    // Perform synchronous request (PeerJS fetch calls are non-critical)
    GError *error = NULL;
    GBytes *response = soup_session_send_and_read(http_session, msg, NULL, &error);

    JSCValue *result = jsc_value_new_object(ctx, NULL, NULL);

    if (error) {
        fprintf(stderr, "[Fetch] Error: %s - %s\n", url, error->message);
        jsc_value_object_set_property(result, "status", jsc_value_new_number(ctx, 0));
        jsc_value_object_set_property(result, "ok", jsc_value_new_boolean(ctx, FALSE));
        jsc_value_object_set_property(result, "body", jsc_value_new_string(ctx, ""));
        g_error_free(error);
    } else {
        guint status = soup_message_get_status(msg);
        gsize len = 0;
        const char *data = g_bytes_get_data(response, &len);
        char *body_str = g_strndup(data, len);

        jsc_value_object_set_property(result, "status", jsc_value_new_number(ctx, status));
        jsc_value_object_set_property(result, "ok", jsc_value_new_boolean(ctx, status >= 200 && status < 300));

        JSCValue *body_val = jsc_value_new_string(ctx, body_str);
        jsc_value_object_set_property(result, "body", body_val);
        g_object_unref(body_val);
        g_free(body_str);
        g_bytes_unref(response);
    }

    g_object_unref(msg);
    g_free(url);
    g_free(method);

    return result;
}

void register_fetch_net_shim(JSCContext *ctx) {
    JSCValue *fn = jsc_value_new_function_variadic(ctx, "__httpFetch",
        G_CALLBACK(native_http_fetch), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__httpFetch", fn);
    g_object_unref(fn);

    // Override fetch to try network first, fall back to file-based fetch
    jsc_context_evaluate(ctx,
        "(function() {"
        "  var _fileFetch = window.fetch;" // save existing file-based fetch
        "  window.fetch = function(url, opts) {"
        "    var urlStr = (typeof url === 'object' && url.href) ? url.href : String(url);"
        "    if (urlStr.indexOf('http://') === 0 || urlStr.indexOf('https://') === 0) {"
        "      return new Promise(function(resolve, reject) {"
        "        try {"
        "          var method = (opts && opts.method) || 'GET';"
        "          var body = (opts && opts.body) || null;"
        "          var result = __httpFetch(urlStr, method, body);"
        "          if (!result) { reject(new Error('Network error')); return; }"
        "          var response = {"
        "            status: result.status,"
        "            ok: result.ok,"
        "            headers: new Map(),"
        "            text: function() { return Promise.resolve(result.body); },"
        "            json: function() { return Promise.resolve(JSON.parse(result.body)); },"
        "            arrayBuffer: function() {"
        "              var enc = new TextEncoder();"
        "              return Promise.resolve(enc.encode(result.body).buffer);"
        "            },"
        "            blob: function() { return Promise.resolve(new Blob([result.body])); }"
        "          };"
        "          resolve(response);"
        "        } catch(e) { reject(e); }"
        "      });"
        "    }"
        "    return _fileFetch ? _fileFetch(url, opts) : Promise.reject(new Error('No fetch handler for: ' + urlStr));"
        "  };"
        "})();"
        , -1);
}
