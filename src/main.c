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

        // Skip non-JavaScript script types (e.g. application/ld+json)
        char *type_attr = strstr(p, "type=");
        if (type_attr && type_attr < tag_end) {
            char tq = type_attr[5]; // quote char
            if (tq == '"' || tq == '\'') {
                char *type_val = type_attr + 6;
                // Only allow text/javascript, application/javascript, module, or no type
                if (strncmp(type_val, "text/javascript", 15) != 0 &&
                    strncmp(type_val, "application/javascript", 22) != 0 &&
                    strncmp(type_val, "module", 6) != 0) {
                    // Skip to end of this script block
                    char *skip_end = strstr(tag_end, "</script>");
                    p = skip_end ? skip_end + 9 : tag_end + 1;
                    continue;
                }
            }
        }

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
        fprintf(stderr, "Usage: %s <game_dir_or_js_file> [width height] [--fullscreen] [--screenshot <frame>] [--phaser <version>]\n", argv[0]);
        fprintf(stderr, "  --phaser <version>  Set Phaser version hint (e.g. 3, 3.19, 4, auto)\n");
        return 1;
    }

    int width = 640, height = 480;
    bool fullscreen = false;
    int screenshot_frame = 0; // 0 = disabled
    const char *phaser_version = "auto"; // auto-detect by default

    // Parse arguments
    const char *input = argv[1];
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--fullscreen") == 0 || strcmp(argv[i], "-f") == 0) {
            fullscreen = true;
        } else if (strcmp(argv[i], "--screenshot") == 0 && i + 1 < argc) {
            screenshot_frame = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--phaser") == 0 && i + 1 < argc) {
            phaser_version = argv[++i];
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
        // Directory: check for index.html first, then game.js
        game_dir = strdup(input);
        char probe[2048];
        snprintf(probe, sizeof(probe), "%s/index.html", game_dir);
        FILE *f = fopen(probe, "r");
        if (f) {
            fclose(f);
            is_html = true;
        } else {
            snprintf(probe, sizeof(probe), "%s/game.js", game_dir);
            f = fopen(probe, "r");
            if (f) {
                fclose(f);
                is_js = true;
            } else {
                is_html = true; // fallback
            }
        }
    }

    // Derive game name from directory name
    char *dir_copy = strdup(game_dir);
    const char *game_name = basename(dir_copy);

    engine_init(width, height, game_dir, game_name, fullscreen);
    free(dir_copy);

    // Set Phaser version hint for runtime
    char phaser_init[256];
    snprintf(phaser_init, sizeof(phaser_init),
        "window.__phaserVersionHint = '%s';", phaser_version);
    JSCValue *pv = jsc_context_evaluate(g_engine.js_ctx, phaser_init, -1);
    if (pv) g_object_unref(pv);

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

        // Parse HTML into litehtml DOM tree and load JS DOM wrapper
        size_t html_len;
        char *html_content = engine_read_file(html_path, &html_len);
        if (html_content) {
            dom_bridge_load_html(g_engine.js_ctx, html_content);
            free(html_content);
        }
        eval_file(g_engine.js_ctx, "runtime/dom.js");

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
        // If input was a directory, load game_dir/game.js; otherwise load the .js file directly
        if (input_len > 3 && strcmp(input + input_len - 3, ".js") == 0) {
            eval_file(g_engine.js_ctx, input);
        } else {
            char js_path[2048];
            snprintf(js_path, sizeof(js_path), "%s/game.js", game_dir);
            eval_file(g_engine.js_ctx, js_path);
        }
    }

    // Detect and report Phaser version, apply version-specific patches
    JSCValue *patch_result = jsc_context_evaluate(g_engine.js_ctx,
        "(function() {"
        "  if (typeof Phaser === 'undefined') return;"
        "  var ver = Phaser.VERSION || 'unknown';"
        "  var hint = window.__phaserVersionHint || 'auto';"
        "  var major = parseInt(ver);"
        "  if (hint !== 'auto') major = parseInt(hint);"
        "  window.__phaserMajor = major;"
        "  console.log('[TinyPhaser] Phaser ' + ver + ' detected (hint: ' + hint + ', major: ' + major + ')');"
        // Phaser 3.x patches
        "  if (major === 3) {"
        "    var A = Phaser.Physics && Phaser.Physics.Arcade;"
        "    if (A) {"
        "      var SB = A.StaticBody && A.StaticBody.prototype;"
        "      if (SB) {"
        "        if (!SB.setCollideWorldBounds) SB.setCollideWorldBounds = function(v) { this.collideWorldBounds = !!v; return this; };"
        "        if (!SB.setImmovable) SB.setImmovable = function() { return this; };"
        "        if (!SB.setAllowGravity) SB.setAllowGravity = function() { return this; };"
        "        if (!SB.setVelocity) SB.setVelocity = function() { return this; };"
        "        if (!SB.setVelocityX) SB.setVelocityX = function() { return this; };"
        "        if (!SB.setVelocityY) SB.setVelocityY = function() { return this; };"
        "        if (!SB.setBounce) SB.setBounce = function() { return this; };"
        "        if (!SB.setBounceX) SB.setBounceX = function() { return this; };"
        "        if (!SB.setBounceY) SB.setBounceY = function() { return this; };"
        "        if (!SB.setAcceleration) SB.setAcceleration = function() { return this; };"
        "        if (!SB.setDrag) SB.setDrag = function() { return this; };"
        "        if (!SB.setFriction) SB.setFriction = function() { return this; };"
        "        if (!SB.setMaxVelocity) SB.setMaxVelocity = function() { return this; };"
        "        if (!SB.setGravity) SB.setGravity = function() { return this; };"
        "        if (!SB.setGravityY) SB.setGravityY = function() { return this; };"
        "        if (!SB.setMass) SB.setMass = function() { return this; };"
        "      }"
        "    }"
        "  }"
        "})();"
        , -1);
    if (patch_result) g_object_unref(patch_result);

    // Fire window.onload if set by game scripts
    JSCValue *onload_result = jsc_context_evaluate(g_engine.js_ctx,
        "if (typeof window.onload === 'function') { window.onload(); }", -1);
    if (onload_result) g_object_unref(onload_result);

    // Main loop
    int frame = 0;
    while (g_engine.running) {
        frame++;
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
                int sw, sh;
                if (g_engine.fbo) {
                    sw = g_engine.render_w; sh = g_engine.render_h;
                    glBindFramebuffer(GL_FRAMEBUFFER, g_engine.fbo);
                } else {
                    sw = g_engine.screen_w; sh = g_engine.screen_h;
                }
                uint8_t *px = malloc(sw * sh * 4);
                glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
                FILE *f = fopen("screenshot.ppm", "wb");
                if (f) {
                    fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                    for (int y = sh-1; y >= 0; y--)
                        for (int x = 0; x < sw; x++) {
                            int i = (y*sw+x)*4;
                            fwrite(px+i, 1, 3, f);
                        }
                    fclose(f);
                    printf("[Engine] Screenshot saved to screenshot.ppm\n");
                }
                free(px);
                continue;
            }
            // Window resize is handled by translate_sdl_event (updates screen_w/h + JS innerWidth/innerHeight)
            translate_sdl_event(&ev);
        }

        // Process timers
        process_timers(now_ms);

        // Fire requestAnimationFrame callbacks
        fire_raf_callbacks(now_ms);

        // Screenshot: capture from FBO (game resolution) or screen
        if (screenshot_frame > 0 && frame == screenshot_frame) {
            int sw, sh;
            if (g_engine.fbo) {
                sw = g_engine.render_w;
                sh = g_engine.render_h;
                glBindFramebuffer(GL_FRAMEBUFFER, g_engine.fbo);
            } else {
                sw = g_engine.screen_w;
                sh = g_engine.screen_h;
            }
            uint8_t *px = malloc(sw * sh * 4);
            glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            FILE *f = fopen("screenshot.ppm", "wb");
            if (f) {
                fprintf(f, "P6\n%d %d\n255\n", sw, sh);
                for (int y = sh-1; y >= 0; y--)
                    for (int x = 0; x < sw; x++) {
                        int i = (y*sw+x)*4;
                        fwrite(px+i, 1, 3, f);
                    }
                fclose(f);
                printf("[Engine] Screenshot saved to screenshot.ppm (frame %d)\n", frame);
            }
            free(px);
            g_engine.running = false;
        }

        // Blit FBO to screen if using offscreen rendering
        engine_blit_fbo();

        SDL_GL_SwapWindow(g_engine.window);
    }

    engine_shutdown();
    free(game_dir);
    return 0;
}
