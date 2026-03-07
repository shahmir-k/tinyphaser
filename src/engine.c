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

void engine_init(int screen_w, int screen_h, const char *game_dir, const char *game_name, bool fullscreen) {
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

    Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
    if (fullscreen) win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;

    char title[256];
    if (game_name && game_name[0])
        snprintf(title, sizeof(title), "TinyPhaser - %s", game_name);
    else
        snprintf(title, sizeof(title), "TinyPhaser");

    e->window = SDL_CreateWindow(title,
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
    register_websocket_shim(e->js_ctx);
    register_webrtc_shim(e->js_ctx);
    register_fetch_net_shim(e->js_ctx);
    register_dom_bridge_shim(e->js_ctx);

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

    dom_bridge_shutdown();
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

void engine_setup_fbo(int render_w, int render_h) {
    Engine *e = &g_engine;

    // Skip if same as current render size
    if (e->render_w == render_w && e->render_h == render_h) return;

    // Clean up existing FBO
    if (e->fbo) {
        glDeleteFramebuffers(1, &e->fbo);
        glDeleteTextures(1, &e->fbo_tex);
        glDeleteRenderbuffers(1, &e->fbo_rbo);
        e->fbo = 0;
    }

    e->render_w = render_w;
    e->render_h = render_h;

    // If render size matches window size, no FBO needed
    if (render_w == e->screen_w && render_h == e->screen_h) {
        printf("[Engine] Render %dx%d = window size, direct rendering\n", render_w, render_h);
        return;
    }

    // Create FBO with color texture + stencil renderbuffer
    glGenFramebuffers(1, &e->fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);

    glGenTextures(1, &e->fbo_tex);
    glBindTexture(GL_TEXTURE_2D, e->fbo_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, render_w, render_h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, e->fbo_tex, 0);

    glGenRenderbuffers(1, &e->fbo_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, e->fbo_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, render_w, render_h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT,
                              GL_RENDERBUFFER, e->fbo_rbo);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[Engine] FBO incomplete: 0x%x\n", status);
        glDeleteFramebuffers(1, &e->fbo);
        glDeleteTextures(1, &e->fbo_tex);
        glDeleteRenderbuffers(1, &e->fbo_rbo);
        e->fbo = 0;
        e->render_w = e->screen_w;
        e->render_h = e->screen_h;
        return;
    }

    printf("[Engine] FBO created: game renders at %dx%d, window is %dx%d\n",
           render_w, render_h, e->screen_w, e->screen_h);
}

// Blit the FBO to the screen, scaling to fit with aspect ratio preserved.
// Uses glBlitFramebuffer to avoid touching shader/texture/buffer state,
// which would invalidate Phaser's internal GL state cache.
void engine_blit_fbo(void) {
    Engine *e = &g_engine;
    if (!e->fbo) return;

    // Calculate letterbox/pillarbox scaling
    float scale_x = (float)e->screen_w / e->render_w;
    float scale_y = (float)e->screen_h / e->render_h;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    int draw_w = (int)(e->render_w * scale);
    int draw_h = (int)(e->render_h * scale);
    int offset_x = (e->screen_w - draw_w) / 2;
    int offset_y = (e->screen_h - draw_h) / 2;

    // Set up read from FBO, draw to screen
    glBindFramebuffer(GL_READ_FRAMEBUFFER, e->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);

    // Clear screen to black (letterbox bars)
    // Save scissor state since Phaser may cache it
    GLboolean scissor_was_on = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor_was_on) glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (scissor_was_on) glEnable(GL_SCISSOR_TEST);

    // Blit FBO to screen with scaling
    glBlitFramebuffer(
        0, 0, e->render_w, e->render_h,
        offset_x, offset_y, offset_x + draw_w, offset_y + draw_h,
        GL_COLOR_BUFFER_BIT, GL_LINEAR);

    // Restore FBO binding for next frame (sets both READ and DRAW)
    glBindFramebuffer(GL_FRAMEBUFFER, e->fbo);
}
