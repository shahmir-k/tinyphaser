#include "engine.h"
#include <string.h>
#include <libgen.h>

// Script entry: either a file path (src != NULL) or inline code (code != NULL)
typedef struct {
    char *src;   // file path (for <script src="...">)
    char *code;  // inline code (for <script>...code...</script>)
} ScriptEntry;

static ScriptEntry *parse_html_scripts(const char *html_path, int *count) {
    size_t len;
    char *html = engine_read_file(html_path, &len);
    if (!html) {
        *count = 0;
        return NULL;
    }

    ScriptEntry *scripts = calloc(64, sizeof(ScriptEntry));
    int n = 0;
    char *p = html;

    while ((p = strstr(p, "<script")) != NULL && n < 64) {
        char *tag_end = strstr(p, ">");
        if (!tag_end) break;

        char *src = strstr(p, "src=");
        if (src && src < tag_end) {
            // External script: <script src="...">
            src += 4;
            char quote = *src;
            if (quote == '"' || quote == '\'') {
                src++;
                char *end_quote = strchr(src, quote);
                if (end_quote) {
                    int slen = end_quote - src;
                    scripts[n].src = malloc(slen + 1);
                    memcpy(scripts[n].src, src, slen);
                    scripts[n].src[slen] = '\0';
                    n++;
                }
            }
        } else {
            // Inline script: <script>...code...</script>
            char *code_start = tag_end + 1;
            char *code_end = strstr(code_start, "</script>");
            if (code_end) {
                int clen = code_end - code_start;
                // Skip if only whitespace
                bool has_content = false;
                for (int i = 0; i < clen; i++) {
                    if (code_start[i] != ' ' && code_start[i] != '\t' &&
                        code_start[i] != '\n' && code_start[i] != '\r') {
                        has_content = true;
                        break;
                    }
                }
                if (has_content) {
                    scripts[n].code = malloc(clen + 1);
                    memcpy(scripts[n].code, code_start, clen);
                    scripts[n].code[clen] = '\0';
                    n++;
                }
                p = code_end + 9; // skip </script>
                continue;
            }
        }
        p = tag_end + 1;
    }

    free(html);
    *count = n;
    return scripts;
}

static void eval_file(JSCContext *ctx, const char *path) {
    size_t len;
    char *source = engine_read_file(path, &len);
    if (!source) {
        fprintf(stderr, "[Engine] Failed to load: %s\n", path);
        return;
    }

    printf("[Engine] Evaluating: %s (%zu bytes)\n", path, len);

    JSCValue *result = jsc_context_evaluate_with_source_uri(ctx, source, len, path, 1);
    if (result) {
        g_object_unref(result);
    }

    JSCException *exc = jsc_context_get_exception(ctx);
    if (exc) {
        fprintf(stderr, "[JS Error] %s: %s\n", path, jsc_exception_get_message(exc));
        jsc_context_clear_exception(ctx);
    }

    free(source);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <game_dir_or_js_file> [width height] [--fullscreen]\n", argv[0]);
        return 1;
    }

    int width = 640, height = 480;
    bool fullscreen = false;

    // Parse arguments
    const char *input = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            fullscreen = true;
        } else if (i == 2 && atoi(argv[i]) > 0) {
            width = atoi(argv[i]);
            if (i + 1 < argc && atoi(argv[i+1]) > 0) {
                height = atoi(argv[++i]);
            }
        }
    }
    char *game_dir = NULL;
    bool is_html = false;
    bool is_js = false;

    // Determine input type
    size_t input_len = strlen(input);
    if (input_len > 5 && strcmp(input + input_len - 5, ".html") == 0) {
        is_html = true;
        char *input_copy = strdup(input);
        game_dir = strdup(dirname(input_copy));
        free(input_copy);
    } else if (input_len > 3 && strcmp(input + input_len - 3, ".js") == 0) {
        is_js = true;
        char *input_copy = strdup(input);
        game_dir = strdup(dirname(input_copy));
        free(input_copy);
    } else {
        // Assume it's a directory containing index.html
        game_dir = strdup(input);
        is_html = true;
    }

    engine_init(width, height, game_dir, fullscreen);

    // Load polyfills
    eval_file(g_engine.js_ctx, "runtime/polyfills.js");

    // Load game scripts
    if (is_html) {
        char html_path[1024];
        if (strcmp(input + input_len - 5, ".html") == 0) {
            snprintf(html_path, sizeof(html_path), "%s", input);
        } else {
            snprintf(html_path, sizeof(html_path), "%s/index.html", game_dir);
        }

        int script_count;
        ScriptEntry *scripts = parse_html_scripts(html_path, &script_count);
        if (scripts) {
            for (int i = 0; i < script_count; i++) {
                if (scripts[i].src) {
                    char full_path[2048];
                    if (scripts[i].src[0] == '/') {
                        snprintf(full_path, sizeof(full_path), "%s", scripts[i].src);
                    } else {
                        snprintf(full_path, sizeof(full_path), "%s/%s", game_dir, scripts[i].src);
                    }
                    eval_file(g_engine.js_ctx, full_path);
                    free(scripts[i].src);
                } else if (scripts[i].code) {
                    printf("[Engine] Evaluating: <inline script> (%zu bytes)\n", strlen(scripts[i].code));
                    JSCValue *r = jsc_context_evaluate_with_source_uri(
                        g_engine.js_ctx, scripts[i].code, -1, html_path, 1);
                    if (r) g_object_unref(r);
                    JSCException *exc = jsc_context_get_exception(g_engine.js_ctx);
                    if (exc) {
                        fprintf(stderr, "[JS Error] inline: %s\n", jsc_exception_get_message(exc));
                        jsc_context_clear_exception(g_engine.js_ctx);
                    }
                    free(scripts[i].code);
                }
            }
            free(scripts);
        }
    } else if (is_js) {
        eval_file(g_engine.js_ctx, input);
    }

    // Main loop
    while (g_engine.running) {
        double now_ms = engine_now_ms();

        // Poll events
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g_engine.running = false;
                break;
            }
            // F11 toggles fullscreen
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F11) {
                Uint32 flags = SDL_GetWindowFlags(g_engine.window);
                SDL_SetWindowFullscreen(g_engine.window,
                    (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                continue;
            }
            // F12 saves screenshot
            if (ev.type == SDL_KEYDOWN && ev.key.keysym.sym == SDLK_F12) {
                uint8_t *px = malloc(width * height * 4);
                glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, px);
                FILE *f = fopen("screenshot.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", width, height);
                    for (int y = height-1; y >= 0; y--)
                        for (int x = 0; x < width; x++) {
                            int i = (y*width+x)*4;
                            fwrite(px+i, 1, 3, f);
                        }
                    fclose(f);
                    printf("[Engine] Screenshot saved to screenshot.ppm\n");
                }
                free(px);
                continue;
            }
            translate_sdl_event(&ev);
        }

        // Process timers
        process_timers(now_ms);

        // Fire requestAnimationFrame callbacks
        fire_raf_callbacks(now_ms);

        SDL_GL_SwapWindow(g_engine.window);
    }

    engine_shutdown();
    free(game_dir);
    return 0;
}
