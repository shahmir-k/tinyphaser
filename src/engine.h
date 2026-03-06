#ifndef ENGINE_H
#define ENGINE_H

#include <jsc/jsc.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_mixer.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

#ifdef USE_GLES2
#include <GLES2/gl2.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#define MAX_TIMERS 256
#define MAX_RAF_CALLBACKS 32

typedef struct {
    int id;
    JSCValue *callback;
    double fire_time_ms;
    double interval_ms; // 0 for setTimeout
    bool active;
} Timer;

typedef struct {
    int id;
    JSCValue *callback;
} RAFCallback;

typedef struct {
    JSCContext *js_ctx;
    SDL_Window *window;
    SDL_GLContext gl_ctx;
    int screen_w;
    int screen_h;
    const char *game_dir;
    bool running;

    // Timer state
    Timer timers[MAX_TIMERS];
    int next_timer_id;

    // requestAnimationFrame state
    RAFCallback raf_callbacks[MAX_RAF_CALLBACKS];
    int raf_count;
    int next_raf_id;

    // Performance timing
    Uint64 perf_freq;
    Uint64 start_time;

    // Canvas/WebGL context JSCValue (kept alive)
    JSCValue *canvas_obj;
    JSCValue *webgl_ctx_obj;
    JSCValue *global_obj;
} Engine;

// Global engine instance
extern Engine g_engine;

// Engine lifecycle
void engine_init(int screen_w, int screen_h, const char *game_dir, bool fullscreen);
void engine_shutdown(void);

// Shim registration
void register_console_shim(JSCContext *ctx);
void register_timers_shim(JSCContext *ctx);
void register_performance_shim(JSCContext *ctx);
void register_window_shim(JSCContext *ctx);
void register_document_shim(JSCContext *ctx);
void register_canvas_shim(JSCContext *ctx);
void register_webgl_shim(JSCContext *ctx);
void register_image_shim(JSCContext *ctx);
void register_xhr_shim(JSCContext *ctx);
void register_events_shim(JSCContext *ctx);
void register_audio_shim(JSCContext *ctx);
void register_text_shim(JSCContext *ctx);
void register_websocket_shim(JSCContext *ctx);
void register_webrtc_shim(JSCContext *ctx);
void register_fetch_net_shim(JSCContext *ctx);

// Timer processing
void process_timers(double now_ms);
void fire_raf_callbacks(double now_ms);

// Event processing
void translate_sdl_event(SDL_Event *event);

// Utility
double engine_now_ms(void);
char *engine_resolve_path(const char *url);
char *engine_read_file(const char *path, size_t *out_len);

#endif
