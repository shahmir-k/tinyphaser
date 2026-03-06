#include "engine.h"

Engine g_engine = {0};

static void js_exception_handler(JSCContext *ctx, JSCException *exception, gpointer user_data) {
    const char *msg = jsc_exception_get_message(exception);
    const char *file = jsc_exception_get_source_uri(exception);
    guint line = jsc_exception_get_line_number(exception);
    fprintf(stderr, "[JS Error] %s:%u: %s\n", file ? file : "<eval>", line, msg);

    char *bt = jsc_exception_get_backtrace_string(exception);
    if (bt) {
        fprintf(stderr, "%s\n", bt);
        g_free(bt);
    }
}

void engine_init(int screen_w, int screen_h, const char *game_dir, bool fullscreen) {
    Engine *e = &g_engine;
    e->screen_w = screen_w;
    e->screen_h = screen_h;
    e->game_dir = game_dir;
    e->running = true;
    e->next_timer_id = 1;
    e->next_raf_id = 1;
    e->perf_freq = SDL_GetPerformanceFrequency();
    e->start_time = SDL_GetPerformanceCounter();

    // SDL2
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_JOYSTICK) < 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        exit(1);
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 0);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);

    Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN;
    if (fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    e->window = SDL_CreateWindow("PhaserQuest",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        screen_w, screen_h,
        win_flags);
    if (!e->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        exit(1);
    }

    e->gl_ctx = SDL_GL_CreateContext(e->window);
    if (!e->gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        exit(1);
    }
    SDL_GL_SetSwapInterval(1);

    // Audio
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) < 0) {
        fprintf(stderr, "Mix_OpenAudio failed: %s\n", Mix_GetError());
    }
    Mix_AllocateChannels(32);

    // Joystick
    if (SDL_NumJoysticks() > 0) {
        SDL_JoystickOpen(0);
    }

    // JSC
    e->js_ctx = jsc_context_new();
    jsc_context_push_exception_handler(e->js_ctx, js_exception_handler, NULL, NULL);
    e->global_obj = jsc_context_get_global_object(e->js_ctx);

    // Register all shims
    register_console_shim(e->js_ctx);
    register_performance_shim(e->js_ctx);
    register_timers_shim(e->js_ctx);
    register_window_shim(e->js_ctx);
    register_document_shim(e->js_ctx);
    register_canvas_shim(e->js_ctx);
    register_webgl_shim(e->js_ctx);
    register_image_shim(e->js_ctx);
    register_xhr_shim(e->js_ctx);
    register_events_shim(e->js_ctx);
    register_audio_shim(e->js_ctx);
    register_text_shim(e->js_ctx);

    printf("[Engine] Initialized: %dx%d, GL: %s\n",
           screen_w, screen_h, glGetString(GL_RENDERER));
}

void engine_shutdown(void) {
    Engine *e = &g_engine;

    // Release JS references
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (e->timers[i].active && e->timers[i].callback) {
            g_object_unref(e->timers[i].callback);
        }
    }
    for (int i = 0; i < e->raf_count; i++) {
        if (e->raf_callbacks[i].callback) {
            g_object_unref(e->raf_callbacks[i].callback);
        }
    }

    if (e->canvas_obj) g_object_unref(e->canvas_obj);
    if (e->webgl_ctx_obj) g_object_unref(e->webgl_ctx_obj);

    g_object_unref(e->js_ctx);

    Mix_CloseAudio();
    TTF_Quit();
    SDL_GL_DeleteContext(e->gl_ctx);
    SDL_DestroyWindow(e->window);
    SDL_Quit();
}

double engine_now_ms(void) {
    Uint64 now = SDL_GetPerformanceCounter();
    return (double)(now - g_engine.start_time) / (double)g_engine.perf_freq * 1000.0;
}
