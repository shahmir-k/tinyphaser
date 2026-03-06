#include "engine.h"

static JSCValue *native_createElement(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_null(ctx);

    char *tag = jsc_value_to_string(g_ptr_array_index(args, 0));

    if (g_ascii_strcasecmp(tag, "canvas") == 0) {
        g_free(tag);
        // Create a new canvas element via JS helper
        JSCValue *r = jsc_context_evaluate(ctx, "__createCanvas()", -1);
        return r;
    }

    // Return a stub element for anything else
    char js[256];
    snprintf(js, sizeof(js), "__createStubElement('%s')", tag);
    g_free(tag);
    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    return r;
}

static JSCValue *native_getElementById(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_null(ctx);

    char *id = jsc_value_to_string(g_ptr_array_index(args, 0));

    // Return a stub div for parent container IDs (Phaser appends canvas to this)
    // Return null for unknown IDs so Phaser falls back to document.body
    JSCValue *result;
    char js[256];
    snprintf(js, sizeof(js), "__createStubElement('div')");
    result = jsc_context_evaluate(ctx, js, -1);

    g_free(id);
    return result;
}

static JSCValue *native_querySelector(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (g_engine.canvas_obj) {
        g_object_ref(g_engine.canvas_obj);
        return g_engine.canvas_obj;
    }
    return jsc_value_new_null(ctx);
}

static JSCValue *native_querySelectorAll(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    return jsc_value_new_array(ctx, G_TYPE_NONE);
}

void register_document_shim(JSCContext *ctx) {
    JSCValue *doc = jsc_value_new_object(ctx, NULL, NULL);

    // Methods
    JSCValue *ce = jsc_value_new_function_variadic(ctx, "createElement",
        G_CALLBACK(native_createElement), NULL, NULL, JSC_TYPE_VALUE);
    JSCValue *gi = jsc_value_new_function_variadic(ctx, "getElementById",
        G_CALLBACK(native_getElementById), NULL, NULL, JSC_TYPE_VALUE);
    JSCValue *qs = jsc_value_new_function_variadic(ctx, "querySelector",
        G_CALLBACK(native_querySelector), NULL, NULL, JSC_TYPE_VALUE);
    JSCValue *qsa = jsc_value_new_function_variadic(ctx, "querySelectorAll",
        G_CALLBACK(native_querySelectorAll), NULL, NULL, JSC_TYPE_VALUE);

    jsc_value_object_set_property(doc, "createElement", ce);
    jsc_value_object_set_property(doc, "getElementById", gi);
    jsc_value_object_set_property(doc, "querySelector", qs);
    jsc_value_object_set_property(doc, "querySelectorAll", qsa);

    g_object_unref(ce);
    g_object_unref(gi);
    g_object_unref(qs);
    g_object_unref(qsa);

    // Properties
    jsc_value_object_set_property(doc, "readyState",
        jsc_value_new_string(ctx, "complete"));
    jsc_value_object_set_property(doc, "visibilityState",
        jsc_value_new_string(ctx, "visible"));
    jsc_value_object_set_property(doc, "hidden",
        jsc_value_new_boolean(ctx, FALSE));

    // body stub
    JSCValue *body = jsc_value_new_object(ctx, NULL, NULL);
    JSCValue *body_style = jsc_value_new_object(ctx, NULL, NULL);
    jsc_value_object_set_property(body, "style", body_style);
    jsc_value_object_set_property(body, "tagName",
        jsc_value_new_string(ctx, "BODY"));
    jsc_value_object_set_property(doc, "body", body);
    jsc_value_object_set_property(doc, "documentElement", body);
    jsc_value_object_set_property(doc, "head", jsc_value_new_object(ctx, NULL, NULL));
    g_object_unref(body_style);
    g_object_unref(body);

    jsc_context_set_value(ctx, "document", doc);
    g_object_unref(doc);

    // Add stub methods via JS for convenience
    // Note: document.addEventListener/removeEventListener are set up in polyfills.js
    // with a working event listener registry for fullscreenchange, pointerlockchange, etc.
    jsc_context_evaluate(ctx,
        "document.addEventListener = function(){};"
        "document.removeEventListener = function(){};"
        "document.createTextNode = function(t){ return { textContent: t }; };"
        "document.createDocumentFragment = function(){ return { appendChild: function(){}, children: [], childNodes: [] }; };"
        "document.body.appendChild = function(el){ return el; };"
        "document.body.removeChild = function(el){ return el; };"
        "document.body.insertBefore = function(el){ return el; };"
        "document.body.contains = function(){ return true; };"
        "document.body.getBoundingClientRect = function(){ return { left:0, top:0, width: innerWidth, height: innerHeight, x:0, y:0 }; };"
        "document.body.addEventListener = function(){};"
        "document.body.removeEventListener = function(){};"
        "document.body.clientWidth = innerWidth || 640;"
        "document.body.clientHeight = innerHeight || 480;"
        "document.head.appendChild = function(el){ return el; };"
        "document.documentElement.clientWidth = innerWidth || 640;"
        "document.documentElement.clientHeight = innerHeight || 480;"
        "document.documentElement.clientTop = 0;"
        "document.documentElement.clientLeft = 0;"
        , -1);
}
