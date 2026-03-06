#include "engine.h"
#include <SDL2/SDL_ttf.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <ctype.h>

// Font registry: maps family name (lowercase) -> file path
static GHashTable *font_paths = NULL;
// Font cache: maps "family:size:style" -> TTF_Font*
static GHashTable *font_cache = NULL;
static bool ttf_initialized = false;

// System font fallbacks
static const char *fallback_sans = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf";
static const char *fallback_sans_bold = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf";
static const char *fallback_mono = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf";
static const char *fallback_mono_bold = "/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf";
static const char *fallback_serif = "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf";
static const char *fallback_serif_bold = "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf";

static char *str_to_lower(const char *s) {
    char *lower = g_strdup(s);
    for (int i = 0; lower[i]; i++) lower[i] = tolower(lower[i]);
    return lower;
}

static void scan_fonts_dir(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;

        char path[2048];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            scan_fonts_dir(path);
            continue;
        }

        // Check for .ttf or .otf extension
        const char *ext = strrchr(ent->d_name, '.');
        if (!ext) continue;
        if (strcasecmp(ext, ".ttf") != 0 && strcasecmp(ext, ".otf") != 0) continue;

        // Extract family name from filename (without extension)
        int namelen = ext - ent->d_name;
        char *family = g_strndup(ent->d_name, namelen);
        char *key = str_to_lower(family);

        if (!g_hash_table_contains(font_paths, key)) {
            g_hash_table_insert(font_paths, key, g_strdup(path));
            printf("[Text] Registered font: '%s' -> %s\n", family, path);
        } else {
            g_free(key);
        }
        g_free(family);
    }
    closedir(d);
}

static const char *resolve_font_path(const char *family, bool bold) {
    // Try exact family name lookup (case-insensitive)
    char *key = str_to_lower(family);
    const char *path = g_hash_table_lookup(font_paths, key);
    g_free(key);
    if (path) return path;

    // Try with -Bold suffix if bold
    if (bold) {
        char *bold_key = g_strdup_printf("%s-bold", family);
        char *lk = str_to_lower(bold_key);
        path = g_hash_table_lookup(font_paths, lk);
        g_free(lk);
        g_free(bold_key);
        if (path) return path;
    }

    // Map standard CSS font families to system fonts
    if (strcasecmp(family, "monospace") == 0 ||
        strcasecmp(family, "Courier") == 0 ||
        strcasecmp(family, "Courier New") == 0) {
        return bold ? fallback_mono_bold : fallback_mono;
    }
    if (strcasecmp(family, "serif") == 0 ||
        strcasecmp(family, "Times") == 0 ||
        strcasecmp(family, "Times New Roman") == 0 ||
        strcasecmp(family, "Georgia") == 0) {
        return bold ? fallback_serif_bold : fallback_serif;
    }

    // Default: sans-serif (covers Arial, Helvetica, sans-serif, etc.)
    return bold ? fallback_sans_bold : fallback_sans;
}

static TTF_Font *get_font(const char *family, int size, bool bold, bool italic) {
    if (!ttf_initialized) return NULL;
    if (size < 1) size = 10;

    // Build cache key
    char cache_key[256];
    snprintf(cache_key, sizeof(cache_key), "%s:%d:%d%d", family, size, bold, italic);
    char *lk = str_to_lower(cache_key);

    TTF_Font *font = g_hash_table_lookup(font_cache, lk);
    if (font) {
        g_free(lk);
        return font;
    }

    const char *path = resolve_font_path(family, bold);
    if (!path) {
        g_free(lk);
        return NULL;
    }

    font = TTF_OpenFont(path, size);
    if (!font) {
        fprintf(stderr, "[Text] Failed to open font '%s' at %dpx from %s: %s\n",
                family, size, path, TTF_GetError());
        g_free(lk);
        return NULL;
    }

    int style = TTF_STYLE_NORMAL;
    if (bold) style |= TTF_STYLE_BOLD;
    if (italic) style |= TTF_STYLE_ITALIC;
    TTF_SetFontStyle(font, style);

    g_hash_table_insert(font_cache, lk, font);
    return font;
}

// __textMeasure(text, family, size, bold, italic) -> {width, height, ascent, descent}
static JSCValue *native_text_measure(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 3) return jsc_value_new_null(ctx);

    char *text = jsc_value_to_string(g_ptr_array_index(args, 0));
    char *family = jsc_value_to_string(g_ptr_array_index(args, 1));
    int size = jsc_value_to_int32(g_ptr_array_index(args, 2));
    bool bold = args->len > 3 ? jsc_value_to_boolean(g_ptr_array_index(args, 3)) : false;
    bool italic = args->len > 4 ? jsc_value_to_boolean(g_ptr_array_index(args, 4)) : false;

    TTF_Font *font = get_font(family, size, bold, italic);
    if (!font) {
        g_free(text);
        g_free(family);
        return jsc_value_new_null(ctx);
    }

    int w = 0, h = 0;
    TTF_SizeUTF8(font, text && text[0] ? text : " ", &w, &h);
    if (!text || !text[0]) w = 0;

    int ascent = TTF_FontAscent(font);
    int descent = TTF_FontDescent(font);

    JSCValue *result = jsc_value_new_object(ctx, NULL, NULL);
    jsc_value_object_set_property(result, "width", jsc_value_new_number(ctx, w));
    jsc_value_object_set_property(result, "height", jsc_value_new_number(ctx, h));
    jsc_value_object_set_property(result, "ascent", jsc_value_new_number(ctx, ascent > 0 ? ascent : h));
    jsc_value_object_set_property(result, "descent", jsc_value_new_number(ctx, descent < 0 ? -descent : 0));

    g_free(text);
    g_free(family);
    return result;
}

// __textRender(text, family, size, bold, italic, r, g, b, a) -> {data: ArrayBuffer, width, height, ascent}
static JSCValue *native_text_render(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 9) return jsc_value_new_null(ctx);

    char *text = jsc_value_to_string(g_ptr_array_index(args, 0));
    char *family = jsc_value_to_string(g_ptr_array_index(args, 1));
    int size = jsc_value_to_int32(g_ptr_array_index(args, 2));
    bool bold = jsc_value_to_boolean(g_ptr_array_index(args, 3));
    bool italic = jsc_value_to_boolean(g_ptr_array_index(args, 4));
    int r = jsc_value_to_int32(g_ptr_array_index(args, 5));
    int g_val = jsc_value_to_int32(g_ptr_array_index(args, 6));
    int b = jsc_value_to_int32(g_ptr_array_index(args, 7));
    int a = jsc_value_to_int32(g_ptr_array_index(args, 8));

    if (!text || !text[0]) {
        g_free(text);
        g_free(family);
        return jsc_value_new_null(ctx);
    }

    TTF_Font *font = get_font(family, size, bold, italic);
    if (!font) {
        fprintf(stderr, "[Text] No font for '%s' %dpx\n", family, size);
        g_free(text);
        g_free(family);
        return jsc_value_new_null(ctx);
    }

    SDL_Color color = { (Uint8)r, (Uint8)g_val, (Uint8)b, (Uint8)a };
    SDL_Surface *surface = TTF_RenderUTF8_Blended(font, text, color);
    if (!surface) {
        fprintf(stderr, "[Text] Render failed: %s\n", TTF_GetError());
        g_free(text);
        g_free(family);
        return jsc_value_new_null(ctx);
    }

    int w = surface->w;
    int h = surface->h;
    int ascent = TTF_FontAscent(font);

    // Convert SDL surface (ARGB) to RGBA
    size_t pixel_size = w * h * 4;
    uint8_t *pixels = g_malloc(pixel_size);

    SDL_LockSurface(surface);
    for (int y = 0; y < h; y++) {
        uint32_t *src_row = (uint32_t *)((uint8_t *)surface->pixels + y * surface->pitch);
        for (int x = 0; x < w; x++) {
            uint32_t pixel = src_row[x];
            int idx = (y * w + x) * 4;
            // SDL_Surface from TTF_RenderUTF8_Blended is in native ARGB format
            pixels[idx]     = (pixel >> 16) & 0xFF; // R
            pixels[idx + 1] = (pixel >> 8) & 0xFF;  // G
            pixels[idx + 2] = pixel & 0xFF;          // B
            pixels[idx + 3] = (pixel >> 24) & 0xFF;  // A
        }
    }
    SDL_UnlockSurface(surface);
    SDL_FreeSurface(surface);

    // Create result object with ArrayBuffer
    JSCValue *pixel_buf = jsc_value_new_array_buffer(ctx, pixels, pixel_size, g_free, pixels);

    JSCValue *result = jsc_value_new_object(ctx, NULL, NULL);
    jsc_value_object_set_property(result, "data", pixel_buf);
    jsc_value_object_set_property(result, "width", jsc_value_new_number(ctx, w));
    jsc_value_object_set_property(result, "height", jsc_value_new_number(ctx, h));
    jsc_value_object_set_property(result, "ascent", jsc_value_new_number(ctx, ascent));

    g_object_unref(pixel_buf);
    g_free(text);
    g_free(family);
    return result;
}

// __textLoadFont(familyName, filePath) -> bool
static JSCValue *native_text_load_font(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_boolean(ctx, FALSE);

    char *family = jsc_value_to_string(g_ptr_array_index(args, 0));
    char *url = jsc_value_to_string(g_ptr_array_index(args, 1));
    char *path = engine_resolve_path(url);
    g_free(url);

    char *key = str_to_lower(family);

    // Verify file exists
    struct stat st;
    if (stat(path, &st) == 0) {
        g_hash_table_insert(font_paths, key, path); // takes ownership of path
        printf("[Text] Loaded font: '%s' -> %s\n", family, path);
        g_free(family);
        return jsc_value_new_boolean(ctx, TRUE);
    }

    fprintf(stderr, "[Text] Font file not found: %s\n", path);
    g_free(key);
    free(path);
    g_free(family);
    return jsc_value_new_boolean(ctx, FALSE);
}

// __textScanFonts(directory)
static void native_text_scan_fonts(GPtrArray *args, gpointer user_data) {
    if (args->len < 1) return;
    char *dir = jsc_value_to_string(g_ptr_array_index(args, 0));
    scan_fonts_dir(dir);
    g_free(dir);
}

static void font_cache_free(gpointer data) {
    TTF_CloseFont((TTF_Font *)data);
}

void register_text_shim(JSCContext *ctx) {
    if (!ttf_initialized) {
        if (TTF_Init() < 0) {
            fprintf(stderr, "[Text] TTF_Init failed: %s\n", TTF_GetError());
            return;
        }
        ttf_initialized = true;
    }

    font_paths = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    font_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, font_cache_free);

    // Auto-scan game directory for fonts
    if (g_engine.game_dir) {
        scan_fonts_dir(g_engine.game_dir);
    }

    JSCValue *fn;

    fn = jsc_value_new_function_variadic(ctx, "__textMeasure",
        G_CALLBACK(native_text_measure), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__textMeasure", fn);
    g_object_unref(fn);

    fn = jsc_value_new_function_variadic(ctx, "__textRender",
        G_CALLBACK(native_text_render), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__textRender", fn);
    g_object_unref(fn);

    fn = jsc_value_new_function_variadic(ctx, "__textLoadFont",
        G_CALLBACK(native_text_load_font), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "__textLoadFont", fn);
    g_object_unref(fn);

    fn = jsc_value_new_function_variadic(ctx, "__textScanFonts",
        G_CALLBACK(native_text_scan_fonts), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__textScanFonts", fn);
    g_object_unref(fn);
}
