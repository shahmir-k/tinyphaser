#include "engine.h"
#include <string.h>
#include <math.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <librsvg/rsvg.h>
#include <cairo.h>

typedef struct {
    int width;
    int height;
    uint8_t *pixels;
    bool complete;
    char *src;
} NativeImage;

static GHashTable *image_table = NULL;
static int next_image_id = 1;

// Rasterize SVG data to RGBA pixels using librsvg+cairo. Returns NULL on failure.
static uint8_t *rasterize_svg(const char *data, size_t len, int *out_w, int *out_h) {
    GError *error = NULL;
    GInputStream *stream = g_memory_input_stream_new_from_data(data, len, NULL);
    RsvgHandle *handle = rsvg_handle_new_from_stream_sync(stream, NULL,
        RSVG_HANDLE_FLAGS_NONE, NULL, &error);
    g_object_unref(stream);
    if (!handle) {
        fprintf(stderr, "[Image] SVG parse failed: %s\n", error ? error->message : "unknown");
        if (error) g_error_free(error);
        return NULL;
    }

    // Get intrinsic dimensions (explicit width/height, then viewBox fallback)
    gdouble svg_w = 0, svg_h = 0;
    gboolean has_w, has_h, has_vbox;
    RsvgLength rw, rh;
    RsvgRectangle vbox;
    rsvg_handle_get_intrinsic_dimensions(handle, &has_w, &rw, &has_h, &rh, &has_vbox, &vbox);
    if (has_w && has_h && rw.length > 1 && rh.length > 1) {
        svg_w = rw.length;
        svg_h = rh.length;
    } else if (has_vbox && vbox.width > 0 && vbox.height > 0) {
        svg_w = vbox.width;
        svg_h = vbox.height;
    }
    // Last resort: query the ink extents
    if (svg_w <= 0 || svg_h <= 0) {
        RsvgRectangle ink;
        if (rsvg_handle_get_geometry_for_element(handle, NULL, &ink, NULL, NULL))  {
            svg_w = ink.width > 0 ? ink.width : 64;
            svg_h = ink.height > 0 ? ink.height : 64;
        } else {
            svg_w = svg_h = 64;
        }
    }

    int w = (int)ceil(svg_w);
    int h = (int)ceil(svg_h);
    if (w <= 0 || h <= 0) { g_object_unref(handle); return NULL; }

    // Scale up small SVGs to reasonable texture size
    double scale = 1.0;
    if (w < 64 || h < 64) {
        scale = 64.0 / (w < h ? w : h);
        w = (int)ceil(svg_w * scale);
        h = (int)ceil(svg_h * scale);
    }

    // Render to cairo surface (ARGB32 = premultiplied BGRA)
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface);

    RsvgRectangle viewport = {0, 0, w, h};
    rsvg_handle_render_document(handle, cr, &viewport, NULL);

    cairo_destroy(cr);
    g_object_unref(handle);
    cairo_surface_flush(surface);

    // Convert from cairo premultiplied BGRA to straight RGBA
    uint8_t *src = cairo_image_surface_get_data(surface);
    int stride = cairo_image_surface_get_stride(surface);
    uint8_t *pixels = malloc(w * h * 4);
    for (int y = 0; y < h; y++) {
        uint8_t *row = src + y * stride;
        for (int x = 0; x < w; x++) {
            int si = x * 4;
            int di = (y * w + x) * 4;
            uint8_t b = row[si + 0];
            uint8_t g = row[si + 1];
            uint8_t r = row[si + 2];
            uint8_t a = row[si + 3];
            // Un-premultiply alpha
            if (a > 0 && a < 255) {
                r = (uint8_t)((r * 255 + a / 2) / a);
                g = (uint8_t)((g * 255 + a / 2) / a);
                b = (uint8_t)((b * 255 + a / 2) / a);
            }
            pixels[di + 0] = r;
            pixels[di + 1] = g;
            pixels[di + 2] = b;
            pixels[di + 3] = a;
        }
    }

    cairo_surface_destroy(surface);

    *out_w = w;
    *out_h = h;
    printf("[Image] Rasterized SVG: %dx%d (scale=%.1f)\n", w, h, scale);
    return pixels;
}

static void native_image_free(gpointer data) {
    NativeImage *img = (NativeImage *)data;
    if (img->pixels) stbi_image_free(img->pixels);
    if (img->src) free(img->src);
    free(img);
}

static JSCValue *native_image_constructor(GPtrArray *args, gpointer user_data) {
    JSCContext *ctx = jsc_context_get_current();

    NativeImage *img = calloc(1, sizeof(NativeImage));
    int id = next_image_id++;

    if (!image_table) {
        image_table = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, native_image_free);
    }
    g_hash_table_insert(image_table, GINT_TO_POINTER(id), img);

    // Create JS object
    JSCValue *obj = jsc_value_new_object(ctx, NULL, NULL);
    jsc_value_object_set_property(obj, "_imageId", jsc_value_new_number(ctx, id));
    jsc_value_object_set_property(obj, "width", jsc_value_new_number(ctx, 0));
    jsc_value_object_set_property(obj, "height", jsc_value_new_number(ctx, 0));
    jsc_value_object_set_property(obj, "naturalWidth", jsc_value_new_number(ctx, 0));
    jsc_value_object_set_property(obj, "naturalHeight", jsc_value_new_number(ctx, 0));
    jsc_value_object_set_property(obj, "complete", jsc_value_new_boolean(ctx, FALSE));
    jsc_value_object_set_property(obj, "crossOrigin", jsc_value_new_string(ctx, ""));

    if (args->len >= 1) {
        jsc_value_object_set_property(obj, "width", g_ptr_array_index(args, 0));
    }
    if (args->len >= 2) {
        jsc_value_object_set_property(obj, "height", g_ptr_array_index(args, 1));
    }

    return obj;
}

// Called from JS when img.src is set (via polyfill setter)
static void native_image_load(GPtrArray *args, gpointer user_data) {
    if (args->len < 2) return;
    JSCContext *ctx = jsc_context_get_current();

    JSCValue *img_obj = g_ptr_array_index(args, 0);
    char *url = jsc_value_to_string(g_ptr_array_index(args, 1));

    // Resolve path
    char *path = engine_resolve_path(url);
    g_free(url);

    // Get image ID
    JSCValue *id_val = jsc_value_object_get_property(img_obj, "_imageId");
    int id = jsc_value_to_int32(id_val);
    g_object_unref(id_val);

    NativeImage *img = g_hash_table_lookup(image_table, GINT_TO_POINTER(id));
    if (!img) { free(path); return; }

    // Load with stb_image, fall back to nanosvg for .svg files
    int w, h, channels;
    uint8_t *pixels = stbi_load(path, &w, &h, &channels, 4);

    if (!pixels) {
        // Try SVG: read file and rasterize
        size_t plen = strlen(path);
        bool is_svg = (plen > 4 && strcasecmp(path + plen - 4, ".svg") == 0);
        if (!is_svg) {
            // Peek at file header
            FILE *f = fopen(path, "rb");
            if (f) {
                char hdr[5] = {0};
                fread(hdr, 1, 4, f);
                fclose(f);
                is_svg = (memcmp(hdr, "<svg", 4) == 0 || memcmp(hdr, "<?xm", 4) == 0);
            }
        }
        if (is_svg) {
            FILE *f = fopen(path, "rb");
            if (f) {
                fseek(f, 0, SEEK_END);
                long sz = ftell(f);
                fseek(f, 0, SEEK_SET);
                char *data = malloc(sz);
                fread(data, 1, sz, f);
                fclose(f);
                pixels = rasterize_svg(data, sz, &w, &h);
                free(data);
            }
        }
    }

    if (pixels) {
        img->width = w;
        img->height = h;
        img->pixels = pixels;
        img->complete = true;

        // Update JS object
        JSCValue *wv = jsc_value_new_number(ctx, w);
        JSCValue *hv = jsc_value_new_number(ctx, h);
        JSCValue *nwv = jsc_value_new_number(ctx, w);
        JSCValue *nhv = jsc_value_new_number(ctx, h);
        JSCValue *cv = jsc_value_new_boolean(ctx, TRUE);
        jsc_value_object_set_property(img_obj, "width", wv);
        jsc_value_object_set_property(img_obj, "height", hv);
        jsc_value_object_set_property(img_obj, "naturalWidth", nwv);
        jsc_value_object_set_property(img_obj, "naturalHeight", nhv);
        jsc_value_object_set_property(img_obj, "complete", cv);
        g_object_unref(wv); g_object_unref(hv);
        g_object_unref(nwv); g_object_unref(nhv);
        g_object_unref(cv);

        // Store pixel data as ArrayBuffer for texImage2D
        // We need to copy because stb_image's buffer lifetime is managed separately
        size_t size = w * h * 4;
        uint8_t *copy = g_memdup2(pixels, size);
        JSCValue *pixel_buf = jsc_value_new_array_buffer(ctx, copy, size, g_free, copy);
        jsc_value_object_set_property(img_obj, "_pixelData", pixel_buf);
        g_object_unref(pixel_buf);

        // Fire onload
        JSCValue *onload = jsc_value_object_get_property(img_obj, "onload");
        if (onload && jsc_value_is_function(onload)) {
            JSCValue *r = jsc_value_function_call(onload, G_TYPE_NONE);
            if (r) g_object_unref(r);
        }
        if (onload) g_object_unref(onload);

        printf("[Image] Loaded: %dx%d\n", w, h);
    } else {
        fprintf(stderr, "[Image] Failed to load: %s\n", path);
        // Fire onerror
        JSCValue *onerror = jsc_value_object_get_property(img_obj, "onerror");
        if (onerror && jsc_value_is_function(onerror)) {
            JSCValue *r = jsc_value_function_call(onerror, G_TYPE_NONE);
            if (r) g_object_unref(r);
        }
        if (onerror) g_object_unref(onerror);
    }

    free(path);
}

// Load image from an ArrayBuffer (for blob URL support)
static void native_image_load_buffer(GPtrArray *args, gpointer user_data) {
    if (args->len < 2) return;
    JSCContext *ctx = jsc_context_get_current();

    JSCValue *img_obj = g_ptr_array_index(args, 0);
    JSCValue *buffer = g_ptr_array_index(args, 1);

    // Get image ID
    JSCValue *id_val = jsc_value_object_get_property(img_obj, "_imageId");
    int id = jsc_value_to_int32(id_val);
    g_object_unref(id_val);

    NativeImage *img = g_hash_table_lookup(image_table, GINT_TO_POINTER(id));
    if (!img) return;

    gsize buf_len = 0;
    void *buf_data = NULL;

    if (jsc_value_is_array_buffer(buffer)) {
        buf_data = jsc_value_array_buffer_get_data(buffer, &buf_len);
    } else if (jsc_value_is_typed_array(buffer)) {
        buf_data = jsc_value_typed_array_get_data(buffer, &buf_len);
    }

    if (!buf_data || buf_len == 0) {
        fprintf(stderr, "[Image] Empty buffer for image decode\n");
        return;
    }

    int w, h, channels;
    uint8_t *pixels = stbi_load_from_memory(buf_data, buf_len, &w, &h, &channels, 4);

    // SVG fallback: if stb_image fails and buffer looks like SVG, rasterize it
    if (!pixels && buf_len >= 4 &&
        (memcmp(buf_data, "<svg", 4) == 0 || memcmp(buf_data, "<?xm", 4) == 0)) {
        pixels = rasterize_svg((const char *)buf_data, buf_len, &w, &h);
    }

    if (pixels) {
        img->width = w;
        img->height = h;
        if (img->pixels) stbi_image_free(img->pixels);
        img->pixels = pixels;
        img->complete = true;

        JSCValue *wv = jsc_value_new_number(ctx, w);
        JSCValue *hv = jsc_value_new_number(ctx, h);
        JSCValue *nwv = jsc_value_new_number(ctx, w);
        JSCValue *nhv = jsc_value_new_number(ctx, h);
        JSCValue *cv = jsc_value_new_boolean(ctx, TRUE);
        jsc_value_object_set_property(img_obj, "width", wv);
        jsc_value_object_set_property(img_obj, "height", hv);
        jsc_value_object_set_property(img_obj, "naturalWidth", nwv);
        jsc_value_object_set_property(img_obj, "naturalHeight", nhv);
        jsc_value_object_set_property(img_obj, "complete", cv);
        g_object_unref(wv); g_object_unref(hv);
        g_object_unref(nwv); g_object_unref(nhv);
        g_object_unref(cv);

        size_t size = w * h * 4;
        uint8_t *copy = g_memdup2(pixels, size);
        JSCValue *pixel_buf = jsc_value_new_array_buffer(ctx, copy, size, g_free, copy);
        jsc_value_object_set_property(img_obj, "_pixelData", pixel_buf);
        g_object_unref(pixel_buf);

        JSCValue *onload = jsc_value_object_get_property(img_obj, "onload");
        if (onload && jsc_value_is_function(onload)) {
            JSCValue *r = jsc_value_function_call(onload, G_TYPE_NONE);
            if (r) g_object_unref(r);
        }
        if (onload) g_object_unref(onload);

        printf("[Image] Loaded from buffer: %dx%d\n", w, h);
    } else {
        fprintf(stderr, "[Image] Failed to decode buffer (%zu bytes)\n", buf_len);
        JSCValue *onerror = jsc_value_object_get_property(img_obj, "onerror");
        if (onerror && jsc_value_is_function(onerror)) {
            JSCValue *r = jsc_value_function_call(onerror, G_TYPE_NONE);
            if (r) g_object_unref(r);
        }
        if (onerror) g_object_unref(onerror);
    }
}

void register_image_shim(JSCContext *ctx) {
    // Image constructor
    JSCValue *ctor = jsc_value_new_function_variadic(ctx, "Image",
        G_CALLBACK(native_image_constructor), NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, "Image", ctor);
    g_object_unref(ctor);

    // Internal load function (file path)
    JSCValue *loader = jsc_value_new_function_variadic(ctx, "__imageLoad",
        G_CALLBACK(native_image_load), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__imageLoad", loader);
    g_object_unref(loader);

    // Internal load function (from ArrayBuffer)
    JSCValue *buf_loader = jsc_value_new_function_variadic(ctx, "__imageLoadBuffer",
        G_CALLBACK(native_image_load_buffer), NULL, NULL, G_TYPE_NONE);
    jsc_context_set_value(ctx, "__imageLoadBuffer", buf_loader);
    g_object_unref(buf_loader);
}
