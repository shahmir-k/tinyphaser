#include "engine.h"

static int native_setTimeout(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return 0;
    JSCValue *callback = g_ptr_array_index(args, 0);
    double delay = args->len > 1 ? jsc_value_to_double(g_ptr_array_index(args, 1)) : 0;

    Engine *e = &g_engine;
    int id = e->next_timer_id++;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!e->timers[i].active) {
            e->timers[i].id = id;
            e->timers[i].callback = callback;
            g_object_ref(callback);
            e->timers[i].fire_time_ms = engine_now_ms() + delay;
            e->timers[i].interval_ms = 0;
            e->timers[i].active = true;
            return id;
        }
    }
    return 0;
}

static int native_setInterval(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return 0;
    JSCValue *callback = g_ptr_array_index(args, 0);
    double interval = args->len > 1 ? jsc_value_to_double(g_ptr_array_index(args, 1)) : 0;

    Engine *e = &g_engine;
    int id = e->next_timer_id++;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!e->timers[i].active) {
            e->timers[i].id = id;
            e->timers[i].callback = callback;
            g_object_ref(callback);
            e->timers[i].fire_time_ms = engine_now_ms() + interval;
            e->timers[i].interval_ms = interval;
            e->timers[i].active = true;
            return id;
        }
    }
    return 0;
}

static void native_clearTimeout(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    Engine *e = &g_engine;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (e->timers[i].active && e->timers[i].id == id) {
            g_object_unref(e->timers[i].callback);
            e->timers[i].active = false;
            return;
        }
    }
}

static int native_requestAnimationFrame(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return 0;
    JSCValue *callback = g_ptr_array_index(args, 0);

    Engine *e = &g_engine;
    if (e->raf_count >= MAX_RAF_CALLBACKS) return 0;

    int id = e->next_raf_id++;
    e->raf_callbacks[e->raf_count].id = id;
    e->raf_callbacks[e->raf_count].callback = callback;
    g_object_ref(callback);
    e->raf_count++;
    return id;
}

static void native_cancelAnimationFrame(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    int id = jsc_value_to_int32(g_ptr_array_index(args, 0));
    Engine *e = &g_engine;

    for (int i = 0; i < e->raf_count; i++) {
        if (e->raf_callbacks[i].id == id) {
            g_object_unref(e->raf_callbacks[i].callback);
            // Shift remaining
            for (int j = i; j < e->raf_count - 1; j++) {
                e->raf_callbacks[j] = e->raf_callbacks[j + 1];
            }
            e->raf_count--;
            return;
        }
    }
}

void process_timers(double now_ms) {
    Engine *e = &g_engine;

    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!e->timers[i].active) continue;
        if (now_ms < e->timers[i].fire_time_ms) continue;

        JSCValue *cb = e->timers[i].callback;

        if (e->timers[i].interval_ms > 0) {
            // setInterval: reschedule
            e->timers[i].fire_time_ms = now_ms + e->timers[i].interval_ms;
        } else {
            // setTimeout: deactivate
            e->timers[i].active = false;
        }

        JSCValue *r = jsc_value_function_call(cb, G_TYPE_NONE);
        if (r) g_object_unref(r);

        // Clear any pending JS exception so it doesn't corrupt subsequent calls
        JSCException *exc = jsc_context_get_exception(e->js_ctx);
        if (exc) jsc_context_clear_exception(e->js_ctx);

        if (e->timers[i].interval_ms == 0) {
            g_object_unref(cb);
        }
    }
}

void fire_raf_callbacks(double now_ms) {
    Engine *e = &g_engine;
    if (e->raf_count == 0) return;

    // Copy callbacks (JS may register new ones during execution)
    int count = e->raf_count;
    RAFCallback cbs[MAX_RAF_CALLBACKS];
    memcpy(cbs, e->raf_callbacks, sizeof(RAFCallback) * count);
    e->raf_count = 0;

    JSCValue *timestamp = jsc_value_new_number(e->js_ctx, now_ms);

    for (int i = 0; i < count; i++) {
        JSCValue *r = jsc_value_function_call(cbs[i].callback, G_TYPE_DOUBLE, now_ms, G_TYPE_NONE);
        if (r) g_object_unref(r);

        // Clear any pending JS exception
        JSCException *exc = jsc_context_get_exception(e->js_ctx);
        if (exc) jsc_context_clear_exception(e->js_ctx);

        g_object_unref(cbs[i].callback);
    }

    g_object_unref(timestamp);
}

void register_timers_shim(JSCContext *ctx) {
    JSCValue *st = jsc_value_new_function_variadic(ctx, "setTimeout",
        G_CALLBACK(native_setTimeout), NULL, NULL, G_TYPE_INT);
    JSCValue *si = jsc_value_new_function_variadic(ctx, "setInterval",
        G_CALLBACK(native_setInterval), NULL, NULL, G_TYPE_INT);
    JSCValue *ct = jsc_value_new_function_variadic(ctx, "clearTimeout",
        G_CALLBACK(native_clearTimeout), NULL, NULL, G_TYPE_NONE);
    JSCValue *ci = jsc_value_new_function_variadic(ctx, "clearInterval",
        G_CALLBACK(native_clearTimeout), NULL, NULL, G_TYPE_NONE);
    JSCValue *raf = jsc_value_new_function_variadic(ctx, "requestAnimationFrame",
        G_CALLBACK(native_requestAnimationFrame), NULL, NULL, G_TYPE_INT);
    JSCValue *caf = jsc_value_new_function_variadic(ctx, "cancelAnimationFrame",
        G_CALLBACK(native_cancelAnimationFrame), NULL, NULL, G_TYPE_NONE);

    jsc_context_set_value(ctx, "setTimeout", st);
    jsc_context_set_value(ctx, "setInterval", si);
    jsc_context_set_value(ctx, "clearTimeout", ct);
    jsc_context_set_value(ctx, "clearInterval", ci);
    jsc_context_set_value(ctx, "requestAnimationFrame", raf);
    jsc_context_set_value(ctx, "cancelAnimationFrame", caf);

    g_object_unref(st);
    g_object_unref(si);
    g_object_unref(ct);
    g_object_unref(ci);
    g_object_unref(raf);
    g_object_unref(caf);
}
