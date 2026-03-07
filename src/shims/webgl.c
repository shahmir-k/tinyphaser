#include "engine.h"
#include <string.h>

// Helper macros for argument extraction
#define ARG(i) ((i) < args->len ? (JSCValue*)g_ptr_array_index(args, (i)) : NULL)
#define ARG_DOUBLE(i) (ARG(i) ? jsc_value_to_double(ARG(i)) : 0.0)
#define ARG_BOOL(i) (ARG(i) ? jsc_value_to_boolean(ARG(i)) : FALSE)
#define CTX() jsc_context_get_current()

// Extract GL integer ID from either a plain number or a wrapper object with _id property.
// This supports both old-style bare integer IDs and new wrapper objects returned by create*.
static GLint gl_arg_int(JSCValue *val) {
    if (!val) return 0;
    if (jsc_value_is_number(val)) return jsc_value_to_int32(val);
    if (jsc_value_is_object(val)) {
        JSCValue *id = jsc_value_object_get_property(val, "_id");
        GLint result = 0;
        if (id && jsc_value_is_number(id)) result = jsc_value_to_int32(id);
        if (id) g_object_unref(id);
        return result;
    }
    return jsc_value_to_int32(val);
}
#define ARG_INT(i) gl_arg_int(ARG(i))

// Create a GL resource wrapper object: { _id: <gl_id> }
// These objects allow Phaser to set arbitrary properties (e.g. __SPECTOR_Metadata).
static JSCValue *gl_wrap_resource(JSCContext *ctx, GLuint id) {
    if (id == 0) return jsc_value_new_null(ctx);
    JSCValue *obj = jsc_value_new_object(ctx, NULL, NULL);
    jsc_value_object_set_property(obj, "_id", jsc_value_new_number(ctx, id));
    return obj;
}

// --- WebGL unpack state (not native GL parameters) ---
static gboolean gl_unpack_flip_y = FALSE;
static gboolean gl_unpack_premultiply_alpha = FALSE;

// Flip pixel data rows in-place for UNPACK_FLIP_Y_WEBGL
static void flip_pixel_rows(uint8_t *data, int width, int height, int channels) {
    int row_bytes = width * channels;
    uint8_t *tmp = malloc(row_bytes);
    for (int y = 0; y < height / 2; y++) {
        uint8_t *top = data + y * row_bytes;
        uint8_t *bot = data + (height - 1 - y) * row_bytes;
        memcpy(tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, tmp, row_bytes);
    }
    free(tmp);
}

// --- State Management ---

static void gl_enable(GPtrArray *args, gpointer ud) { glEnable(ARG_INT(0)); }
static void gl_disable(GPtrArray *args, gpointer ud) { glDisable(ARG_INT(0)); }
static void gl_blendFunc(GPtrArray *args, gpointer ud) { glBlendFunc(ARG_INT(0), ARG_INT(1)); }
static void gl_blendFuncSeparate(GPtrArray *args, gpointer ud) {
    glBlendFuncSeparate(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}
static void gl_blendEquation(GPtrArray *args, gpointer ud) { glBlendEquation(ARG_INT(0)); }
static void gl_blendEquationSeparate(GPtrArray *args, gpointer ud) {
    glBlendEquationSeparate(ARG_INT(0), ARG_INT(1));
}
static void gl_depthFunc(GPtrArray *args, gpointer ud) { glDepthFunc(ARG_INT(0)); }
static void gl_depthMask(GPtrArray *args, gpointer ud) { glDepthMask(ARG_BOOL(0)); }
static void gl_colorMask(GPtrArray *args, gpointer ud) {
    glColorMask(ARG_BOOL(0), ARG_BOOL(1), ARG_BOOL(2), ARG_BOOL(3));
}
static void gl_stencilFunc(GPtrArray *args, gpointer ud) { glStencilFunc(ARG_INT(0), ARG_INT(1), ARG_INT(2)); }
static void gl_stencilMask(GPtrArray *args, gpointer ud) { glStencilMask(ARG_INT(0)); }
static void gl_stencilOp(GPtrArray *args, gpointer ud) { glStencilOp(ARG_INT(0), ARG_INT(1), ARG_INT(2)); }
static void gl_cullFace(GPtrArray *args, gpointer ud) { glCullFace(ARG_INT(0)); }
static void gl_frontFace(GPtrArray *args, gpointer ud) { glFrontFace(ARG_INT(0)); }
static void gl_lineWidth(GPtrArray *args, gpointer ud) { glLineWidth(ARG_DOUBLE(0)); }
static void gl_pixelStorei(GPtrArray *args, gpointer ud) {
    int pname = ARG_INT(0);
    if (pname == 0x9240) { gl_unpack_flip_y = ARG_BOOL(1); return; }
    if (pname == 0x9241) { gl_unpack_premultiply_alpha = ARG_BOOL(1); return; }
    if (pname == 0x9243) return; // UNPACK_COLORSPACE_CONVERSION_WEBGL: ignore
    glPixelStorei(pname, ARG_INT(1));
}

// --- Clear ---

static void gl_clearColor(GPtrArray *args, gpointer ud) {
    glClearColor(ARG_DOUBLE(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3));
}
static void gl_clearDepthf(GPtrArray *args, gpointer ud) { glClearDepthf(ARG_DOUBLE(0)); }
static void gl_clearStencil(GPtrArray *args, gpointer ud) { glClearStencil(ARG_INT(0)); }
static void gl_clear(GPtrArray *args, gpointer ud) { glClear(ARG_INT(0)); }

// --- Viewport / Scissor ---

static void gl_viewport(GPtrArray *args, gpointer ud) {
    glViewport(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}
static void gl_scissor(GPtrArray *args, gpointer ud) {
    glScissor(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}

// --- Shaders ---

static JSCValue *gl_createShader(GPtrArray *args, gpointer ud) {
    GLuint s = glCreateShader(ARG_INT(0));
    return gl_wrap_resource(CTX(), s);
}

static void gl_shaderSource(GPtrArray *args, gpointer ud) {
    GLuint shader = ARG_INT(0);
    char *src = jsc_value_to_string(ARG(1));
    const char *srcs[] = { src };
    glShaderSource(shader, 1, srcs, NULL);
    g_free(src);
}

static void gl_compileShader(GPtrArray *args, gpointer ud) {
    GLuint shader = ARG_INT(0);
    glCompileShader(shader);
    GLint status;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
    if (!status) {
        GLint len;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            char *log = malloc(len);
            glGetShaderInfoLog(shader, len, NULL, log);
            fprintf(stderr, "[WebGL] Shader %u compile failed: %s\n", shader, log);
            free(log);
        }
    }
}

static JSCValue *gl_getShaderParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetShaderiv(ARG_INT(0), ARG_INT(1), &val);
    if (ARG_INT(1) == GL_COMPILE_STATUS || ARG_INT(1) == GL_DELETE_STATUS)
        return jsc_value_new_boolean(CTX(), val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getShaderInfoLog(GPtrArray *args, gpointer ud) {
    GLint len;
    glGetShaderiv(ARG_INT(0), GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return jsc_value_new_string(CTX(), "");
    char *buf = malloc(len);
    glGetShaderInfoLog(ARG_INT(0), len, NULL, buf);
    JSCValue *r = jsc_value_new_string(CTX(), buf);
    free(buf);
    return r;
}

static void gl_deleteShader(GPtrArray *args, gpointer ud) { glDeleteShader(ARG_INT(0)); }

// --- Programs ---

static JSCValue *gl_createProgram(GPtrArray *args, gpointer ud) {
    GLuint p = glCreateProgram();
    return gl_wrap_resource(CTX(), p);
}

static void gl_attachShader(GPtrArray *args, gpointer ud) { glAttachShader(ARG_INT(0), ARG_INT(1)); }
static void gl_linkProgram(GPtrArray *args, gpointer ud) {
    GLuint prog = ARG_INT(0);
    glLinkProgram(prog);
    GLint status;
    glGetProgramiv(prog, GL_LINK_STATUS, &status);
    if (!status) {
        GLint len;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        if (len > 0) {
            char *log = malloc(len);
            glGetProgramInfoLog(prog, len, NULL, log);
            fprintf(stderr, "[WebGL] Program %u link failed: %s\n", prog, log);
            free(log);
        }
    }
}
static void gl_useProgram(GPtrArray *args, gpointer ud) {
    if (ARG(0) && jsc_value_is_null(ARG(0))) glUseProgram(0);
    else glUseProgram(ARG_INT(0));
}

static JSCValue *gl_getProgramParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetProgramiv(ARG_INT(0), ARG_INT(1), &val);
    if (ARG_INT(1) == GL_LINK_STATUS || ARG_INT(1) == GL_VALIDATE_STATUS ||
        ARG_INT(1) == GL_DELETE_STATUS)
        return jsc_value_new_boolean(CTX(), val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getProgramInfoLog(GPtrArray *args, gpointer ud) {
    GLint len;
    glGetProgramiv(ARG_INT(0), GL_INFO_LOG_LENGTH, &len);
    if (len <= 0) return jsc_value_new_string(CTX(), "");
    char *buf = malloc(len);
    glGetProgramInfoLog(ARG_INT(0), len, NULL, buf);
    JSCValue *r = jsc_value_new_string(CTX(), buf);
    free(buf);
    return r;
}

static void gl_deleteProgram(GPtrArray *args, gpointer ud) { glDeleteProgram(ARG_INT(0)); }
static void gl_validateProgram(GPtrArray *args, gpointer ud) { glValidateProgram(ARG_INT(0)); }

static void gl_bindAttribLocation(GPtrArray *args, gpointer ud) {
    char *name = jsc_value_to_string(ARG(2));
    glBindAttribLocation(ARG_INT(0), ARG_INT(1), name);
    g_free(name);
}

// --- Attributes ---

static JSCValue *gl_getAttribLocation(GPtrArray *args, gpointer ud) {
    char *name = jsc_value_to_string(ARG(1));
    GLint loc = glGetAttribLocation(ARG_INT(0), name);
    g_free(name);
    return jsc_value_new_number(CTX(), loc);
}

static void gl_enableVertexAttribArray(GPtrArray *args, gpointer ud) { glEnableVertexAttribArray(ARG_INT(0)); }
static void gl_disableVertexAttribArray(GPtrArray *args, gpointer ud) { glDisableVertexAttribArray(ARG_INT(0)); }

static void gl_vertexAttribPointer(GPtrArray *args, gpointer ud) {
    glVertexAttribPointer(
        ARG_INT(0), ARG_INT(1), ARG_INT(2),
        ARG_BOOL(3), ARG_INT(4),
        (const void *)(intptr_t)ARG_INT(5));
}

static void gl_vertexAttrib1f(GPtrArray *args, gpointer ud) { glVertexAttrib1f(ARG_INT(0), ARG_DOUBLE(1)); }
static void gl_vertexAttrib2f(GPtrArray *args, gpointer ud) { glVertexAttrib2f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2)); }
static void gl_vertexAttrib3f(GPtrArray *args, gpointer ud) { glVertexAttrib3f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3)); }
static void gl_vertexAttrib4f(GPtrArray *args, gpointer ud) { glVertexAttrib4f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3), ARG_DOUBLE(4)); }

// --- Uniforms ---

static JSCValue *gl_getUniformLocation(GPtrArray *args, gpointer ud) {
    char *name = jsc_value_to_string(ARG(1));
    GLint loc = glGetUniformLocation(ARG_INT(0), name);
    g_free(name);
    if (loc == -1) return jsc_value_new_null(CTX());
    return jsc_value_new_number(CTX(), loc);
}

static void gl_uniform1i(GPtrArray *args, gpointer ud) { glUniform1i(ARG_INT(0), ARG_INT(1)); }
static void gl_uniform1f(GPtrArray *args, gpointer ud) { glUniform1f(ARG_INT(0), ARG_DOUBLE(1)); }
static void gl_uniform2f(GPtrArray *args, gpointer ud) { glUniform2f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2)); }
static void gl_uniform3f(GPtrArray *args, gpointer ud) { glUniform3f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3)); }
static void gl_uniform4f(GPtrArray *args, gpointer ud) { glUniform4f(ARG_INT(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3), ARG_DOUBLE(4)); }
static void gl_uniform2i(GPtrArray *args, gpointer ud) { glUniform2i(ARG_INT(0), ARG_INT(1), ARG_INT(2)); }
static void gl_uniform3i(GPtrArray *args, gpointer ud) { glUniform3i(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3)); }
static void gl_uniform4i(GPtrArray *args, gpointer ud) { glUniform4i(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3), ARG_INT(4)); }

static void gl_uniform1fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        glUniform1fv(ARG_INT(0), len, data);
    }
}
static void gl_uniform2fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        glUniform2fv(ARG_INT(0), len / 2, data);
    }
}
static void gl_uniform3fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        glUniform3fv(ARG_INT(0), len / 3, data);
    }
}
static void gl_uniform4fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        glUniform4fv(ARG_INT(0), len / 4, data);
    }
}

static void gl_uniform1iv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        int *data = (int *)jsc_value_typed_array_get_data(arr, &len);
        glUniform1iv(ARG_INT(0), len, data);
    }
}
static void gl_uniform2iv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        int *data = (int *)jsc_value_typed_array_get_data(arr, &len);
        glUniform2iv(ARG_INT(0), len / 2, data);
    }
}
static void gl_uniform3iv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        int *data = (int *)jsc_value_typed_array_get_data(arr, &len);
        glUniform3iv(ARG_INT(0), len / 3, data);
    }
}
static void gl_uniform4iv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(1);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        int *data = (int *)jsc_value_typed_array_get_data(arr, &len);
        glUniform4iv(ARG_INT(0), len / 4, data);
    }
}

static void gl_uniformMatrix2fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(2);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        int count = len / 4; if (count < 1) count = 1;
        glUniformMatrix2fv(ARG_INT(0), count, ARG_BOOL(1), data);
    }
}
static void gl_uniformMatrix3fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(2);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        int count = len / 9; if (count < 1) count = 1;
        glUniformMatrix3fv(ARG_INT(0), count, ARG_BOOL(1), data);
    }
}
static void gl_uniformMatrix4fv(GPtrArray *args, gpointer ud) {
    JSCValue *arr = ARG(2);
    if (jsc_value_is_typed_array(arr)) {
        gsize len;
        float *data = jsc_value_typed_array_get_data(arr, &len);
        int count = len / 16;
        if (count < 1) count = 1;
        glUniformMatrix4fv(ARG_INT(0), count, ARG_BOOL(1), data);
    } else if (jsc_value_is_array(arr)) {
        float mat[16];
        for (int i = 0; i < 16; i++) {
            JSCValue *v = jsc_value_object_get_property_at_index(arr, i);
            mat[i] = jsc_value_to_double(v);
            g_object_unref(v);
        }
        glUniformMatrix4fv(ARG_INT(0), 1, ARG_BOOL(1), mat);
    }
}

// --- Buffers ---

static JSCValue *gl_createBuffer(GPtrArray *args, gpointer ud) {
    GLuint buf;
    glGenBuffers(1, &buf);
    return gl_wrap_resource(CTX(), buf);
}

static void gl_bindBuffer(GPtrArray *args, gpointer ud) {
    GLuint buf = 0;
    if (ARG(1) && !jsc_value_is_null(ARG(1))) buf = ARG_INT(1);
    glBindBuffer(ARG_INT(0), buf);
}

static void gl_bufferData(GPtrArray *args, gpointer ud) {
    int target = ARG_INT(0);
    JSCValue *data = ARG(1);
    int usage = ARG_INT(2);

    if (jsc_value_is_typed_array(data)) {
        gsize size;
        void *ptr = jsc_value_typed_array_get_data(data, &size);
        size = jsc_value_typed_array_get_size(data);
        glBufferData(target, size, ptr, usage);
    } else if (jsc_value_is_array_buffer(data)) {
        gsize size;
        void *ptr = jsc_value_array_buffer_get_data(data, &size);
        glBufferData(target, size, ptr, usage);
    } else if (jsc_value_is_number(data)) {
        glBufferData(target, jsc_value_to_int32(data), NULL, usage);
    }
}

static void gl_bufferSubData(GPtrArray *args, gpointer ud) {
    int target = ARG_INT(0);
    int offset = ARG_INT(1);
    JSCValue *data = ARG(2);

    if (jsc_value_is_typed_array(data)) {
        gsize size;
        void *ptr = jsc_value_typed_array_get_data(data, &size);
        size = jsc_value_typed_array_get_size(data);
        glBufferSubData(target, offset, size, ptr);
    } else if (jsc_value_is_array_buffer(data)) {
        gsize size;
        void *ptr = jsc_value_array_buffer_get_data(data, &size);
        glBufferSubData(target, offset, size, ptr);
    }
}

static void gl_deleteBuffer(GPtrArray *args, gpointer ud) {
    GLuint buf = ARG_INT(0);
    glDeleteBuffers(1, &buf);
}

// --- Textures ---

static JSCValue *gl_createTexture(GPtrArray *args, gpointer ud) {
    GLuint tex;
    glGenTextures(1, &tex);
    return gl_wrap_resource(CTX(), tex);
}

static void gl_bindTexture(GPtrArray *args, gpointer ud) {
    GLuint tex = 0;
    if (ARG(1) && !jsc_value_is_null(ARG(1))) tex = ARG_INT(1);
    glBindTexture(ARG_INT(0), tex);
}

static void gl_activeTexture(GPtrArray *args, gpointer ud) { glActiveTexture(ARG_INT(0)); }

static void gl_texParameteri(GPtrArray *args, gpointer ud) {
    glTexParameteri(ARG_INT(0), ARG_INT(1), ARG_INT(2));
}

static void gl_texParameterf(GPtrArray *args, gpointer ud) {
    glTexParameterf(ARG_INT(0), ARG_INT(1), ARG_DOUBLE(2));
}

// Get bytes-per-pixel for a GL format
static int gl_format_channels(int format) {
    switch (format) {
        case GL_RGBA: return 4;
        case GL_RGB: return 3;
        case GL_LUMINANCE_ALPHA: return 2;
        case GL_LUMINANCE: case GL_ALPHA: return 1;
        default: return 4;
    }
}

static void gl_texImage2D(GPtrArray *args, gpointer ud) {
    int target = ARG_INT(0);
    int level = ARG_INT(1);
    int internalformat = ARG_INT(2);

    if (args->len == 9) {
        // texImage2D(target, level, internalformat, width, height, border, format, type, data)
        int width = ARG_INT(3);
        int height = ARG_INT(4);
        int format = ARG_INT(5 + 1); // skip border
        int type = ARG_INT(6 + 1);
        JSCValue *data = ARG(7 + 1);
        void *pixels = NULL;
        if (data && jsc_value_is_typed_array(data)) {
            gsize len;
            pixels = jsc_value_typed_array_get_data(data, &len);
        }
        if (gl_unpack_flip_y && pixels && width > 0 && height > 0) {
            int channels = gl_format_channels(format);
            gsize size = width * height * channels;
            uint8_t *copy = malloc(size);
            memcpy(copy, pixels, size);
            flip_pixel_rows(copy, width, height, channels);
            glTexImage2D(target, level, internalformat, width, height, 0, format, type, copy);
            free(copy);
        } else {
            glTexImage2D(target, level, internalformat, width, height, 0, format, type, pixels);
        }
    } else if (args->len == 6) {
        // texImage2D(target, level, internalformat, format, type, image_or_canvas)
        int format = ARG_INT(3);
        int type = ARG_INT(4);
        JSCValue *source = ARG(5);

        if (source && jsc_value_is_object(source)) {
            // Check if it's an Image with _pixelData
            JSCValue *pd = jsc_value_object_get_property(source, "_pixelData");
            JSCValue *wv = jsc_value_object_get_property(source, "width");
            JSCValue *hv = jsc_value_object_get_property(source, "height");

            if (pd && jsc_value_is_array_buffer(pd)) {
                int w = jsc_value_to_int32(wv);
                int h = jsc_value_to_int32(hv);
                gsize size;
                void *pixels = jsc_value_array_buffer_get_data(pd, &size);
                if (gl_unpack_flip_y && pixels && w > 0 && h > 0) {
                    int channels = gl_format_channels(format);
                    gsize copy_size = w * h * channels;
                    uint8_t *copy = malloc(copy_size);
                    memcpy(copy, pixels, copy_size < size ? copy_size : size);
                    flip_pixel_rows(copy, w, h, channels);
                    glTexImage2D(target, level, internalformat, w, h, 0, format, type, copy);
                    free(copy);
                } else {
                    glTexImage2D(target, level, internalformat, w, h, 0, format, type, pixels);
                }
            }

            if (pd) g_object_unref(pd);
            if (wv) g_object_unref(wv);
            if (hv) g_object_unref(hv);
        }
    }
}

static void gl_texSubImage2D(GPtrArray *args, gpointer ud) {
    int target = ARG_INT(0);
    int level = ARG_INT(1);
    int xoff = ARG_INT(2);
    int yoff = ARG_INT(3);

    if (args->len == 9) {
        int width = ARG_INT(4);
        int height = ARG_INT(5);
        int format = ARG_INT(6);
        int type = ARG_INT(7);
        JSCValue *data = ARG(8);
        void *pixels = NULL;
        if (data && jsc_value_is_typed_array(data)) {
            gsize len;
            pixels = jsc_value_typed_array_get_data(data, &len);
        }
        if (gl_unpack_flip_y && pixels && width > 0 && height > 0) {
            int channels = gl_format_channels(format);
            gsize size = width * height * channels;
            uint8_t *copy = malloc(size);
            memcpy(copy, pixels, size);
            flip_pixel_rows(copy, width, height, channels);
            glTexSubImage2D(target, level, xoff, yoff, width, height, format, type, copy);
            free(copy);
        } else {
            glTexSubImage2D(target, level, xoff, yoff, width, height, format, type, pixels);
        }
    } else if (args->len == 7) {
        int format = ARG_INT(4);
        int type = ARG_INT(5);
        JSCValue *source = ARG(6);

        if (source && jsc_value_is_object(source)) {
            JSCValue *pd = jsc_value_object_get_property(source, "_pixelData");
            JSCValue *wv = jsc_value_object_get_property(source, "width");
            JSCValue *hv = jsc_value_object_get_property(source, "height");

            if (pd && jsc_value_is_array_buffer(pd)) {
                int w = jsc_value_to_int32(wv);
                int h = jsc_value_to_int32(hv);
                gsize size;
                void *pixels = jsc_value_array_buffer_get_data(pd, &size);
                if (gl_unpack_flip_y && pixels && w > 0 && h > 0) {
                    int channels = gl_format_channels(format);
                    gsize copy_size = w * h * channels;
                    uint8_t *copy = malloc(copy_size);
                    memcpy(copy, pixels, copy_size < size ? copy_size : size);
                    flip_pixel_rows(copy, w, h, channels);
                    glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, copy);
                    free(copy);
                } else {
                    glTexSubImage2D(target, level, xoff, yoff, w, h, format, type, pixels);
                }
            }

            if (pd) g_object_unref(pd);
            if (wv) g_object_unref(wv);
            if (hv) g_object_unref(hv);
        }
    }
}

static void gl_generateMipmap(GPtrArray *args, gpointer ud) { glGenerateMipmap(ARG_INT(0)); }

static void gl_deleteTexture(GPtrArray *args, gpointer ud) {
    GLuint tex = ARG_INT(0);
    glDeleteTextures(1, &tex);
}

// --- Framebuffers ---

static JSCValue *gl_createFramebuffer(GPtrArray *args, gpointer ud) {
    GLuint fbo;
    glGenFramebuffers(1, &fbo);
    return gl_wrap_resource(CTX(), fbo);
}

static void gl_bindFramebuffer(GPtrArray *args, gpointer ud) {
    GLuint fbo = 0;
    if (ARG(1) && !jsc_value_is_null(ARG(1))) fbo = ARG_INT(1);
    // Redirect "bind to screen" (fbo=0) to our offscreen FBO if active
    if (fbo == 0 && g_engine.fbo) fbo = g_engine.fbo;
    glBindFramebuffer(ARG_INT(0), fbo);
}

static void gl_framebufferTexture2D(GPtrArray *args, gpointer ud) {
    glFramebufferTexture2D(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3), ARG_INT(4));
}

static JSCValue *gl_checkFramebufferStatus(GPtrArray *args, gpointer ud) {
    return jsc_value_new_number(CTX(), glCheckFramebufferStatus(ARG_INT(0)));
}

static void gl_deleteFramebuffer(GPtrArray *args, gpointer ud) {
    GLuint fbo = ARG_INT(0);
    glDeleteFramebuffers(1, &fbo);
}

// --- Renderbuffers ---

static JSCValue *gl_createRenderbuffer(GPtrArray *args, gpointer ud) {
    GLuint rbo;
    glGenRenderbuffers(1, &rbo);
    return gl_wrap_resource(CTX(), rbo);
}

static void gl_bindRenderbuffer(GPtrArray *args, gpointer ud) {
    GLuint rbo = 0;
    if (ARG(1) && !jsc_value_is_null(ARG(1))) rbo = ARG_INT(1);
    glBindRenderbuffer(ARG_INT(0), rbo);
}

static void gl_renderbufferStorage(GPtrArray *args, gpointer ud) {
    glRenderbufferStorage(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}

static void gl_framebufferRenderbuffer(GPtrArray *args, gpointer ud) {
    glFramebufferRenderbuffer(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}

static void gl_deleteRenderbuffer(GPtrArray *args, gpointer ud) {
    GLuint rbo = ARG_INT(0);
    glDeleteRenderbuffers(1, &rbo);
}

// --- Draw ---

static void gl_drawArrays(GPtrArray *args, gpointer ud) {
    glDrawArrays(ARG_INT(0), ARG_INT(1), ARG_INT(2));
}

static void gl_drawElements(GPtrArray *args, gpointer ud) {
    GLint count = ARG_INT(1);
    // Safety: skip draw calls with zero or negative count
    if (count <= 0) return;
    // Validate we have a bound element buffer
    GLint ebo = 0;
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING, &ebo);
    if (ebo == 0) return; // No index buffer bound - skip draw
    glDrawElements(ARG_INT(0), count, ARG_INT(2), (const void *)(intptr_t)ARG_INT(3));
}

// --- Query ---

static JSCValue *gl_getError(GPtrArray *args, gpointer ud) {
    return jsc_value_new_number(CTX(), glGetError());
}

static JSCValue *gl_getParameter(GPtrArray *args, gpointer ud) {
    GLenum pname = ARG_INT(0);
    switch (pname) {
        case GL_MAX_TEXTURE_SIZE: {
            GLint v; glGetIntegerv(pname, &v);
            return jsc_value_new_number(CTX(), v);
        }
        case GL_MAX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS:
        case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS:
        case GL_MAX_VERTEX_ATTRIBS:
        case GL_MAX_RENDERBUFFER_SIZE:
        case GL_MAX_VIEWPORT_DIMS: {
            GLint v; glGetIntegerv(pname, &v);
            return jsc_value_new_number(CTX(), v);
        }
        case GL_RENDERER:
            return jsc_value_new_string(CTX(), (const char *)glGetString(GL_RENDERER));
        case GL_VENDOR:
            return jsc_value_new_string(CTX(), (const char *)glGetString(GL_VENDOR));
        case GL_VERSION:
            return jsc_value_new_string(CTX(), (const char *)glGetString(GL_VERSION));
        case GL_SHADING_LANGUAGE_VERSION:
            return jsc_value_new_string(CTX(), (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION));
        default: {
            GLint v; glGetIntegerv(pname, &v);
            return jsc_value_new_number(CTX(), v);
        }
    }
}

// --- WebGL Extension implementations ---

static JSCValue *gl_drawArraysInstanced(GPtrArray *args, gpointer ud) {
    glDrawArraysInstanced(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
    return jsc_value_new_undefined(CTX());
}

static JSCValue *gl_drawElementsInstanced(GPtrArray *args, gpointer ud) {
    glDrawElementsInstanced(ARG_INT(0), ARG_INT(1), ARG_INT(2),
        (const void *)(intptr_t)ARG_INT(3), ARG_INT(4));
    return jsc_value_new_undefined(CTX());
}

static JSCValue *gl_vertexAttribDivisor(GPtrArray *args, gpointer ud) {
    glVertexAttribDivisor(ARG_INT(0), ARG_INT(1));
    return jsc_value_new_undefined(CTX());
}

static JSCValue *gl_createVertexArray(GPtrArray *args, gpointer ud) {
    GLuint vao;
    glGenVertexArrays(1, &vao);
    return gl_wrap_resource(CTX(), vao);
}

static JSCValue *gl_deleteVertexArray(GPtrArray *args, gpointer ud) {
    GLuint vao = ARG_INT(0);
    glDeleteVertexArrays(1, &vao);
    return jsc_value_new_undefined(CTX());
}

static JSCValue *gl_bindVertexArray(GPtrArray *args, gpointer ud) {
    GLuint vao = 0;
    if (ARG(0) && !jsc_value_is_null(ARG(0))) vao = ARG_INT(0);
    glBindVertexArray(vao);
    return jsc_value_new_undefined(CTX());
}

static JSCValue *gl_isVertexArray(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsVertexArray(ARG_INT(0)));
}

static JSCValue *gl_getExtension(GPtrArray *args, gpointer ud) {
    if (args->len < 1) return jsc_value_new_null(CTX());
    char *name = jsc_value_to_string(ARG(0));
    JSCContext *ctx = CTX();
    JSCValue *ext = NULL;

    if (strcmp(name, "ANGLE_instanced_arrays") == 0) {
        ext = jsc_value_new_object(ctx, NULL, NULL);
        JSCValue *f1 = jsc_value_new_function_variadic(ctx, "drawArraysInstancedANGLE",
            G_CALLBACK(gl_drawArraysInstanced), NULL, NULL, JSC_TYPE_VALUE);
        JSCValue *f2 = jsc_value_new_function_variadic(ctx, "drawElementsInstancedANGLE",
            G_CALLBACK(gl_drawElementsInstanced), NULL, NULL, JSC_TYPE_VALUE);
        JSCValue *f3 = jsc_value_new_function_variadic(ctx, "vertexAttribDivisorANGLE",
            G_CALLBACK(gl_vertexAttribDivisor), NULL, NULL, JSC_TYPE_VALUE);
        jsc_value_object_set_property(ext, "drawArraysInstancedANGLE", f1);
        jsc_value_object_set_property(ext, "drawElementsInstancedANGLE", f2);
        jsc_value_object_set_property(ext, "vertexAttribDivisorANGLE", f3);
        jsc_value_object_set_property(ext, "VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE",
            jsc_value_new_number(ctx, 0x88FE));
        g_object_unref(f1); g_object_unref(f2); g_object_unref(f3);
    } else if (strcmp(name, "OES_vertex_array_object") == 0) {
        ext = jsc_value_new_object(ctx, NULL, NULL);
        JSCValue *f1 = jsc_value_new_function_variadic(ctx, "createVertexArrayOES",
            G_CALLBACK(gl_createVertexArray), NULL, NULL, JSC_TYPE_VALUE);
        JSCValue *f2 = jsc_value_new_function_variadic(ctx, "deleteVertexArrayOES",
            G_CALLBACK(gl_deleteVertexArray), NULL, NULL, JSC_TYPE_VALUE);
        JSCValue *f3 = jsc_value_new_function_variadic(ctx, "bindVertexArrayOES",
            G_CALLBACK(gl_bindVertexArray), NULL, NULL, JSC_TYPE_VALUE);
        JSCValue *f4 = jsc_value_new_function_variadic(ctx, "isVertexArrayOES",
            G_CALLBACK(gl_isVertexArray), NULL, NULL, JSC_TYPE_VALUE);
        jsc_value_object_set_property(ext, "createVertexArrayOES", f1);
        jsc_value_object_set_property(ext, "deleteVertexArrayOES", f2);
        jsc_value_object_set_property(ext, "bindVertexArrayOES", f3);
        jsc_value_object_set_property(ext, "isVertexArrayOES", f4);
        jsc_value_object_set_property(ext, "VERTEX_ARRAY_BINDING_OES",
            jsc_value_new_number(ctx, 0x85B5));
        g_object_unref(f1); g_object_unref(f2); g_object_unref(f3); g_object_unref(f4);
    } else if (strcmp(name, "OES_element_index_uint") == 0 ||
               strcmp(name, "OES_standard_derivatives") == 0 ||
               strcmp(name, "OES_texture_float") == 0 ||
               strcmp(name, "OES_texture_half_float") == 0 ||
               strcmp(name, "OES_texture_float_linear") == 0 ||
               strcmp(name, "OES_texture_half_float_linear") == 0 ||
               strcmp(name, "WEBGL_depth_texture") == 0 ||
               strcmp(name, "EXT_blend_minmax") == 0 ||
               strcmp(name, "EXT_frag_depth") == 0 ||
               strcmp(name, "EXT_shader_texture_lod") == 0 ||
               strcmp(name, "WEBGL_lose_context") == 0) {
        // These extensions just need a truthy return value (no methods needed)
        ext = jsc_value_new_object(ctx, NULL, NULL);
        // HALF_FLOAT_OES constant needed by OES_texture_half_float
        if (strcmp(name, "OES_texture_half_float") == 0) {
            jsc_value_object_set_property(ext, "HALF_FLOAT_OES",
                jsc_value_new_number(ctx, 0x8D61));
        }
        if (strcmp(name, "EXT_blend_minmax") == 0) {
            jsc_value_object_set_property(ext, "MIN_EXT", jsc_value_new_number(ctx, 0x8007));
            jsc_value_object_set_property(ext, "MAX_EXT", jsc_value_new_number(ctx, 0x8008));
        }
        if (strcmp(name, "WEBGL_lose_context") == 0) {
            JSCValue *dummy = jsc_context_evaluate(ctx,
                "(function(){ return { loseContext: function(){}, restoreContext: function(){} }; })()", -1);
            g_free(name);
            return dummy;
        }
    }

    g_free(name);
    return ext ? ext : jsc_value_new_null(ctx);
}

static JSCValue *gl_getSupportedExtensions(GPtrArray *args, gpointer ud) {
    JSCContext *ctx = CTX();
    return jsc_context_evaluate(ctx,
        "['ANGLE_instanced_arrays','OES_vertex_array_object','OES_element_index_uint',"
        "'OES_standard_derivatives','OES_texture_float','OES_texture_half_float',"
        "'OES_texture_float_linear','OES_texture_half_float_linear',"
        "'WEBGL_depth_texture','EXT_blend_minmax','EXT_frag_depth',"
        "'EXT_shader_texture_lod','WEBGL_lose_context']", -1);
}

static JSCValue *gl_isContextLost(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), FALSE);
}

static void gl_readPixels(GPtrArray *args, gpointer ud) {
    JSCValue *data = ARG(6);
    if (data && jsc_value_is_typed_array(data)) {
        gsize len;
        void *ptr = jsc_value_typed_array_get_data(data, &len);
        glReadPixels(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3),
                     ARG_INT(4), ARG_INT(5), ptr);
    }
}

static void gl_flush(GPtrArray *args, gpointer ud) { glFlush(); }
static void gl_finish(GPtrArray *args, gpointer ud) { glFinish(); }
static void gl_hint(GPtrArray *args, gpointer ud) { glHint(ARG_INT(0), ARG_INT(1)); }

static JSCValue *gl_isEnabled(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsEnabled(ARG_INT(0)));
}

static void gl_blendColor(GPtrArray *args, gpointer ud) {
    glBlendColor(ARG_DOUBLE(0), ARG_DOUBLE(1), ARG_DOUBLE(2), ARG_DOUBLE(3));
}

static void gl_depthRange(GPtrArray *args, gpointer ud) {
    glDepthRangef(ARG_DOUBLE(0), ARG_DOUBLE(1));
}

static void gl_polygonOffset(GPtrArray *args, gpointer ud) {
    glPolygonOffset(ARG_DOUBLE(0), ARG_DOUBLE(1));
}

static void gl_sampleCoverage(GPtrArray *args, gpointer ud) {
    glSampleCoverage(ARG_DOUBLE(0), ARG_BOOL(1));
}

static void gl_stencilFuncSeparate(GPtrArray *args, gpointer ud) {
    glStencilFuncSeparate(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}

static void gl_stencilMaskSeparate(GPtrArray *args, gpointer ud) {
    glStencilMaskSeparate(ARG_INT(0), ARG_INT(1));
}

static void gl_stencilOpSeparate(GPtrArray *args, gpointer ud) {
    glStencilOpSeparate(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3));
}

static JSCValue *gl_getBufferParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetBufferParameteriv(ARG_INT(0), ARG_INT(1), &val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getTexParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetTexParameteriv(ARG_INT(0), ARG_INT(1), &val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getVertexAttrib(GPtrArray *args, gpointer ud) {
    GLenum pname = ARG_INT(1);
    if (pname == GL_CURRENT_VERTEX_ATTRIB) {
        GLfloat v[4];
        glGetVertexAttribfv(ARG_INT(0), pname, v);
        JSCValue *arr = jsc_value_new_typed_array(CTX(), JSC_TYPED_ARRAY_FLOAT32, 4);
        gsize sz;
        float *data = jsc_value_typed_array_get_data(arr, &sz);
        memcpy(data, v, 16);
        return arr;
    }
    GLint val;
    glGetVertexAttribiv(ARG_INT(0), pname, &val);
    if (pname == GL_VERTEX_ATTRIB_ARRAY_ENABLED || pname == GL_VERTEX_ATTRIB_ARRAY_NORMALIZED)
        return jsc_value_new_boolean(CTX(), val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getVertexAttribOffset(GPtrArray *args, gpointer ud) {
    GLvoid *ptr;
    glGetVertexAttribPointerv(ARG_INT(0), ARG_INT(1), &ptr);
    return jsc_value_new_number(CTX(), (intptr_t)ptr);
}

static JSCValue *gl_getUniform(GPtrArray *args, gpointer ud) {
    // Return a float value for the uniform (simplified)
    GLfloat val;
    glGetUniformfv(ARG_INT(0), ARG_INT(1), &val);
    return jsc_value_new_number(CTX(), val);
}

static void gl_copyTexImage2D(GPtrArray *args, gpointer ud) {
    glCopyTexImage2D(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3),
                     ARG_INT(4), ARG_INT(5), ARG_INT(6), ARG_INT(7));
}

static void gl_copyTexSubImage2D(GPtrArray *args, gpointer ud) {
    glCopyTexSubImage2D(ARG_INT(0), ARG_INT(1), ARG_INT(2), ARG_INT(3),
                        ARG_INT(4), ARG_INT(5), ARG_INT(6), ARG_INT(7));
}

static JSCValue *gl_isTexture(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsTexture(ARG_INT(0)));
}
static JSCValue *gl_isBuffer(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsBuffer(ARG_INT(0)));
}
static JSCValue *gl_isFramebuffer(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsFramebuffer(ARG_INT(0)));
}
static JSCValue *gl_isRenderbuffer(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsRenderbuffer(ARG_INT(0)));
}
static JSCValue *gl_isProgram(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsProgram(ARG_INT(0)));
}
static JSCValue *gl_isShader(GPtrArray *args, gpointer ud) {
    return jsc_value_new_boolean(CTX(), glIsShader(ARG_INT(0)));
}

static JSCValue *gl_getShaderSource(GPtrArray *args, gpointer ud) {
    GLint len;
    glGetShaderiv(ARG_INT(0), GL_SHADER_SOURCE_LENGTH, &len);
    if (len <= 0) return jsc_value_new_string(CTX(), "");
    char *buf = malloc(len);
    glGetShaderSource(ARG_INT(0), len, NULL, buf);
    JSCValue *r = jsc_value_new_string(CTX(), buf);
    free(buf);
    return r;
}

static JSCValue *gl_getShaderPrecisionFormat(GPtrArray *args, gpointer ud) {
    GLint range[2], prec;
    glGetShaderPrecisionFormat(ARG_INT(0), ARG_INT(1), range, &prec);
    JSCValue *obj = jsc_value_new_object(CTX(), NULL, NULL);
    jsc_value_object_set_property(obj, "rangeMin", jsc_value_new_number(CTX(), range[0]));
    jsc_value_object_set_property(obj, "rangeMax", jsc_value_new_number(CTX(), range[1]));
    jsc_value_object_set_property(obj, "precision", jsc_value_new_number(CTX(), prec));
    return obj;
}

static JSCValue *gl_getRenderbufferParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetRenderbufferParameteriv(ARG_INT(0), ARG_INT(1), &val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getFramebufferAttachmentParameter(GPtrArray *args, gpointer ud) {
    GLint val;
    glGetFramebufferAttachmentParameteriv(ARG_INT(0), ARG_INT(1), ARG_INT(2), &val);
    return jsc_value_new_number(CTX(), val);
}

static JSCValue *gl_getActiveAttrib(GPtrArray *args, gpointer ud) {
    char name[256];
    GLsizei length;
    GLint size;
    GLenum type;
    glGetActiveAttrib(ARG_INT(0), ARG_INT(1), sizeof(name), &length, &size, &type, name);
    JSCValue *info = jsc_value_new_object(CTX(), NULL, NULL);
    jsc_value_object_set_property(info, "name", jsc_value_new_string(CTX(), name));
    jsc_value_object_set_property(info, "size", jsc_value_new_number(CTX(), size));
    jsc_value_object_set_property(info, "type", jsc_value_new_number(CTX(), type));
    return info;
}

static JSCValue *gl_getActiveUniform(GPtrArray *args, gpointer ud) {
    char name[256];
    GLsizei length;
    GLint size;
    GLenum type;
    glGetActiveUniform(ARG_INT(0), ARG_INT(1), sizeof(name), &length, &size, &type, name);
    JSCValue *info = jsc_value_new_object(CTX(), NULL, NULL);
    jsc_value_object_set_property(info, "name", jsc_value_new_string(CTX(), name));
    jsc_value_object_set_property(info, "size", jsc_value_new_number(CTX(), size));
    jsc_value_object_set_property(info, "type", jsc_value_new_number(CTX(), type));
    return info;
}

// --- Registration ---

#define ADD_GL_VOID(name, cb) do { \
    JSCValue *f = jsc_value_new_function_variadic(ctx, #name, G_CALLBACK(cb), NULL, NULL, G_TYPE_NONE); \
    jsc_value_object_set_property(gl, #name, f); \
    g_object_unref(f); \
} while(0)

#define ADD_GL_VAL(name, cb) do { \
    JSCValue *f = jsc_value_new_function_variadic(ctx, #name, G_CALLBACK(cb), NULL, NULL, JSC_TYPE_VALUE); \
    jsc_value_object_set_property(gl, #name, f); \
    g_object_unref(f); \
} while(0)

#define ADD_GL_CONST(name, val) do { \
    jsc_value_object_set_property(gl, #name, jsc_value_new_number(ctx, val)); \
} while(0)

void register_webgl_shim(JSCContext *ctx) {
    JSCValue *gl = jsc_value_new_object(ctx, NULL, NULL);

    // --- Functions ---
    // State
    ADD_GL_VOID(enable, gl_enable);
    ADD_GL_VOID(disable, gl_disable);
    ADD_GL_VOID(blendFunc, gl_blendFunc);
    ADD_GL_VOID(blendFuncSeparate, gl_blendFuncSeparate);
    ADD_GL_VOID(blendEquation, gl_blendEquation);
    ADD_GL_VOID(blendEquationSeparate, gl_blendEquationSeparate);
    ADD_GL_VOID(depthFunc, gl_depthFunc);
    ADD_GL_VOID(depthMask, gl_depthMask);
    ADD_GL_VOID(colorMask, gl_colorMask);
    ADD_GL_VOID(stencilFunc, gl_stencilFunc);
    ADD_GL_VOID(stencilMask, gl_stencilMask);
    ADD_GL_VOID(stencilOp, gl_stencilOp);
    ADD_GL_VOID(cullFace, gl_cullFace);
    ADD_GL_VOID(frontFace, gl_frontFace);
    ADD_GL_VOID(lineWidth, gl_lineWidth);
    ADD_GL_VOID(pixelStorei, gl_pixelStorei);

    // Clear
    ADD_GL_VOID(clearColor, gl_clearColor);
    ADD_GL_VOID(clearDepth, gl_clearDepthf);
    ADD_GL_VOID(clearStencil, gl_clearStencil);
    ADD_GL_VOID(clear, gl_clear);

    // Viewport
    ADD_GL_VOID(viewport, gl_viewport);
    ADD_GL_VOID(scissor, gl_scissor);

    // Shaders
    ADD_GL_VAL(createShader, gl_createShader);
    ADD_GL_VOID(shaderSource, gl_shaderSource);
    ADD_GL_VOID(compileShader, gl_compileShader);
    ADD_GL_VAL(getShaderParameter, gl_getShaderParameter);
    ADD_GL_VAL(getShaderInfoLog, gl_getShaderInfoLog);
    ADD_GL_VOID(deleteShader, gl_deleteShader);

    // Programs
    ADD_GL_VAL(createProgram, gl_createProgram);
    ADD_GL_VOID(attachShader, gl_attachShader);
    ADD_GL_VOID(linkProgram, gl_linkProgram);
    ADD_GL_VOID(useProgram, gl_useProgram);
    ADD_GL_VAL(getProgramParameter, gl_getProgramParameter);
    ADD_GL_VAL(getProgramInfoLog, gl_getProgramInfoLog);
    ADD_GL_VOID(deleteProgram, gl_deleteProgram);
    ADD_GL_VOID(validateProgram, gl_validateProgram);
    ADD_GL_VOID(bindAttribLocation, gl_bindAttribLocation);

    // Attributes
    ADD_GL_VAL(getAttribLocation, gl_getAttribLocation);
    ADD_GL_VOID(enableVertexAttribArray, gl_enableVertexAttribArray);
    ADD_GL_VOID(disableVertexAttribArray, gl_disableVertexAttribArray);
    ADD_GL_VOID(vertexAttribPointer, gl_vertexAttribPointer);
    ADD_GL_VOID(vertexAttrib1f, gl_vertexAttrib1f);
    ADD_GL_VOID(vertexAttrib2f, gl_vertexAttrib2f);
    ADD_GL_VOID(vertexAttrib3f, gl_vertexAttrib3f);
    ADD_GL_VOID(vertexAttrib4f, gl_vertexAttrib4f);

    // Uniforms
    ADD_GL_VAL(getUniformLocation, gl_getUniformLocation);
    ADD_GL_VOID(uniform1i, gl_uniform1i);
    ADD_GL_VOID(uniform1f, gl_uniform1f);
    ADD_GL_VOID(uniform2f, gl_uniform2f);
    ADD_GL_VOID(uniform3f, gl_uniform3f);
    ADD_GL_VOID(uniform4f, gl_uniform4f);
    ADD_GL_VOID(uniform2i, gl_uniform2i);
    ADD_GL_VOID(uniform3i, gl_uniform3i);
    ADD_GL_VOID(uniform4i, gl_uniform4i);
    ADD_GL_VOID(uniform1fv, gl_uniform1fv);
    ADD_GL_VOID(uniform2fv, gl_uniform2fv);
    ADD_GL_VOID(uniform3fv, gl_uniform3fv);
    ADD_GL_VOID(uniform4fv, gl_uniform4fv);
    ADD_GL_VOID(uniform1iv, gl_uniform1iv);
    ADD_GL_VOID(uniform2iv, gl_uniform2iv);
    ADD_GL_VOID(uniform3iv, gl_uniform3iv);
    ADD_GL_VOID(uniform4iv, gl_uniform4iv);
    ADD_GL_VOID(uniformMatrix2fv, gl_uniformMatrix2fv);
    ADD_GL_VOID(uniformMatrix3fv, gl_uniformMatrix3fv);
    ADD_GL_VOID(uniformMatrix4fv, gl_uniformMatrix4fv);

    // Buffers
    ADD_GL_VAL(createBuffer, gl_createBuffer);
    ADD_GL_VOID(bindBuffer, gl_bindBuffer);
    ADD_GL_VOID(bufferData, gl_bufferData);
    ADD_GL_VOID(bufferSubData, gl_bufferSubData);
    ADD_GL_VOID(deleteBuffer, gl_deleteBuffer);

    // Textures
    ADD_GL_VAL(createTexture, gl_createTexture);
    ADD_GL_VOID(bindTexture, gl_bindTexture);
    ADD_GL_VOID(activeTexture, gl_activeTexture);
    ADD_GL_VOID(texParameteri, gl_texParameteri);
    ADD_GL_VOID(texParameterf, gl_texParameterf);
    ADD_GL_VOID(texImage2D, gl_texImage2D);
    ADD_GL_VOID(texSubImage2D, gl_texSubImage2D);
    ADD_GL_VOID(generateMipmap, gl_generateMipmap);
    ADD_GL_VOID(deleteTexture, gl_deleteTexture);

    // Framebuffers
    ADD_GL_VAL(createFramebuffer, gl_createFramebuffer);
    ADD_GL_VOID(bindFramebuffer, gl_bindFramebuffer);
    ADD_GL_VOID(framebufferTexture2D, gl_framebufferTexture2D);
    ADD_GL_VAL(checkFramebufferStatus, gl_checkFramebufferStatus);
    ADD_GL_VOID(deleteFramebuffer, gl_deleteFramebuffer);

    // Renderbuffers
    ADD_GL_VAL(createRenderbuffer, gl_createRenderbuffer);
    ADD_GL_VOID(bindRenderbuffer, gl_bindRenderbuffer);
    ADD_GL_VOID(renderbufferStorage, gl_renderbufferStorage);
    ADD_GL_VOID(framebufferRenderbuffer, gl_framebufferRenderbuffer);
    ADD_GL_VOID(deleteRenderbuffer, gl_deleteRenderbuffer);

    // Draw
    ADD_GL_VOID(drawArrays, gl_drawArrays);
    ADD_GL_VOID(drawElements, gl_drawElements);

    // State (additional)
    ADD_GL_VAL(isEnabled, gl_isEnabled);
    ADD_GL_VOID(blendColor, gl_blendColor);
    ADD_GL_VOID(depthRange, gl_depthRange);
    ADD_GL_VOID(polygonOffset, gl_polygonOffset);
    ADD_GL_VOID(sampleCoverage, gl_sampleCoverage);
    ADD_GL_VOID(stencilFuncSeparate, gl_stencilFuncSeparate);
    ADD_GL_VOID(stencilMaskSeparate, gl_stencilMaskSeparate);
    ADD_GL_VOID(stencilOpSeparate, gl_stencilOpSeparate);

    // Copy
    ADD_GL_VOID(copyTexImage2D, gl_copyTexImage2D);
    ADD_GL_VOID(copyTexSubImage2D, gl_copyTexSubImage2D);

    // Query
    ADD_GL_VAL(getError, gl_getError);
    ADD_GL_VAL(getParameter, gl_getParameter);
    ADD_GL_VAL(getExtension, gl_getExtension);
    ADD_GL_VAL(getSupportedExtensions, gl_getSupportedExtensions);
    ADD_GL_VAL(isContextLost, gl_isContextLost);
    ADD_GL_VOID(readPixels, gl_readPixels);
    ADD_GL_VOID(flush, gl_flush);
    ADD_GL_VOID(finish, gl_finish);
    ADD_GL_VOID(hint, gl_hint);
    ADD_GL_VAL(getActiveAttrib, gl_getActiveAttrib);
    ADD_GL_VAL(getActiveUniform, gl_getActiveUniform);
    ADD_GL_VAL(getBufferParameter, gl_getBufferParameter);
    ADD_GL_VAL(getTexParameter, gl_getTexParameter);
    ADD_GL_VAL(getVertexAttrib, gl_getVertexAttrib);
    ADD_GL_VAL(getVertexAttribOffset, gl_getVertexAttribOffset);
    ADD_GL_VAL(getUniform, gl_getUniform);
    ADD_GL_VAL(getShaderSource, gl_getShaderSource);
    ADD_GL_VAL(getShaderPrecisionFormat, gl_getShaderPrecisionFormat);
    ADD_GL_VAL(getRenderbufferParameter, gl_getRenderbufferParameter);
    ADD_GL_VAL(getFramebufferAttachmentParameter, gl_getFramebufferAttachmentParameter);
    ADD_GL_VAL(isTexture, gl_isTexture);
    ADD_GL_VAL(isBuffer, gl_isBuffer);
    ADD_GL_VAL(isFramebuffer, gl_isFramebuffer);
    ADD_GL_VAL(isRenderbuffer, gl_isRenderbuffer);
    ADD_GL_VAL(isProgram, gl_isProgram);
    ADD_GL_VAL(isShader, gl_isShader);

    // WebGL 2 VAO methods (also available as OES extension)
    ADD_GL_VAL(createVertexArray, gl_createVertexArray);
    ADD_GL_VOID(deleteVertexArray, gl_deleteVertexArray);
    ADD_GL_VOID(bindVertexArray, gl_bindVertexArray);

    // --- WebGL Constants ---
    // Data types
    ADD_GL_CONST(BYTE, 0x1400);
    ADD_GL_CONST(UNSIGNED_BYTE, 0x1401);
    ADD_GL_CONST(SHORT, 0x1402);
    ADD_GL_CONST(UNSIGNED_SHORT, 0x1403);
    ADD_GL_CONST(INT, 0x1404);
    ADD_GL_CONST(UNSIGNED_INT, 0x1405);
    ADD_GL_CONST(FLOAT, 0x1406);

    // Buffer targets
    ADD_GL_CONST(ARRAY_BUFFER, 0x8892);
    ADD_GL_CONST(ELEMENT_ARRAY_BUFFER, 0x8893);

    // Buffer usage
    ADD_GL_CONST(STATIC_DRAW, 0x88E4);
    ADD_GL_CONST(DYNAMIC_DRAW, 0x88E8);
    ADD_GL_CONST(STREAM_DRAW, 0x88E0);

    // Texture targets
    ADD_GL_CONST(TEXTURE_2D, 0x0DE1);
    ADD_GL_CONST(TEXTURE_CUBE_MAP, 0x8513);
    ADD_GL_CONST(TEXTURE_CUBE_MAP_POSITIVE_X, 0x8515);

    // Texture parameters
    ADD_GL_CONST(TEXTURE_MIN_FILTER, 0x2801);
    ADD_GL_CONST(TEXTURE_MAG_FILTER, 0x2800);
    ADD_GL_CONST(TEXTURE_WRAP_S, 0x2802);
    ADD_GL_CONST(TEXTURE_WRAP_T, 0x2803);
    ADD_GL_CONST(NEAREST, 0x2600);
    ADD_GL_CONST(LINEAR, 0x2601);
    ADD_GL_CONST(NEAREST_MIPMAP_NEAREST, 0x2700);
    ADD_GL_CONST(LINEAR_MIPMAP_NEAREST, 0x2701);
    ADD_GL_CONST(NEAREST_MIPMAP_LINEAR, 0x2702);
    ADD_GL_CONST(LINEAR_MIPMAP_LINEAR, 0x2703);
    ADD_GL_CONST(CLAMP_TO_EDGE, 0x812F);
    ADD_GL_CONST(REPEAT, 0x2901);
    ADD_GL_CONST(MIRRORED_REPEAT, 0x8370);

    // Texture units
    ADD_GL_CONST(TEXTURE0, 0x84C0);
    ADD_GL_CONST(TEXTURE1, 0x84C1);
    ADD_GL_CONST(TEXTURE2, 0x84C2);
    ADD_GL_CONST(TEXTURE3, 0x84C3);
    ADD_GL_CONST(TEXTURE4, 0x84C4);
    ADD_GL_CONST(TEXTURE5, 0x84C5);
    ADD_GL_CONST(TEXTURE6, 0x84C6);
    ADD_GL_CONST(TEXTURE7, 0x84C7);
    ADD_GL_CONST(TEXTURE8, 0x84C8);
    ADD_GL_CONST(TEXTURE9, 0x84C9);
    ADD_GL_CONST(TEXTURE10, 0x84CA);
    ADD_GL_CONST(TEXTURE11, 0x84CB);
    ADD_GL_CONST(TEXTURE12, 0x84CC);
    ADD_GL_CONST(TEXTURE13, 0x84CD);
    ADD_GL_CONST(TEXTURE14, 0x84CE);
    ADD_GL_CONST(TEXTURE15, 0x84CF);

    // Pixel formats
    ADD_GL_CONST(ALPHA, 0x1906);
    ADD_GL_CONST(RGB, 0x1907);
    ADD_GL_CONST(RGBA, 0x1908);
    ADD_GL_CONST(LUMINANCE, 0x1909);
    ADD_GL_CONST(LUMINANCE_ALPHA, 0x190A);

    // Shader types
    ADD_GL_CONST(VERTEX_SHADER, 0x8B31);
    ADD_GL_CONST(FRAGMENT_SHADER, 0x8B30);
    ADD_GL_CONST(COMPILE_STATUS, 0x8B81);
    ADD_GL_CONST(LINK_STATUS, 0x8B82);
    ADD_GL_CONST(VALIDATE_STATUS, 0x8B83);
    ADD_GL_CONST(DELETE_STATUS, 0x8B80);
    ADD_GL_CONST(SHADER_TYPE, 0x8B4F);
    ADD_GL_CONST(ACTIVE_UNIFORMS, 0x8B86);
    ADD_GL_CONST(ACTIVE_ATTRIBUTES, 0x8B89);

    // Clear bits
    ADD_GL_CONST(COLOR_BUFFER_BIT, 0x4000);
    ADD_GL_CONST(DEPTH_BUFFER_BIT, 0x0100);
    ADD_GL_CONST(STENCIL_BUFFER_BIT, 0x0400);

    // Draw modes
    ADD_GL_CONST(POINTS, 0x0000);
    ADD_GL_CONST(LINES, 0x0001);
    ADD_GL_CONST(LINE_LOOP, 0x0002);
    ADD_GL_CONST(LINE_STRIP, 0x0003);
    ADD_GL_CONST(TRIANGLES, 0x0004);
    ADD_GL_CONST(TRIANGLE_STRIP, 0x0005);
    ADD_GL_CONST(TRIANGLE_FAN, 0x0006);

    // Enable caps
    ADD_GL_CONST(BLEND, 0x0BE2);
    ADD_GL_CONST(DEPTH_TEST, 0x0B71);
    ADD_GL_CONST(STENCIL_TEST, 0x0B90);
    ADD_GL_CONST(SCISSOR_TEST, 0x0C11);
    ADD_GL_CONST(CULL_FACE, 0x0B44);

    // Blend factors
    ADD_GL_CONST(ZERO, 0);
    ADD_GL_CONST(ONE, 1);
    ADD_GL_CONST(SRC_COLOR, 0x0300);
    ADD_GL_CONST(ONE_MINUS_SRC_COLOR, 0x0301);
    ADD_GL_CONST(SRC_ALPHA, 0x0302);
    ADD_GL_CONST(ONE_MINUS_SRC_ALPHA, 0x0303);
    ADD_GL_CONST(DST_ALPHA, 0x0304);
    ADD_GL_CONST(ONE_MINUS_DST_ALPHA, 0x0305);
    ADD_GL_CONST(DST_COLOR, 0x0306);
    ADD_GL_CONST(ONE_MINUS_DST_COLOR, 0x0307);
    ADD_GL_CONST(SRC_ALPHA_SATURATE, 0x0308);

    // Blend equations
    ADD_GL_CONST(FUNC_ADD, 0x8006);
    ADD_GL_CONST(FUNC_SUBTRACT, 0x800A);
    ADD_GL_CONST(FUNC_REVERSE_SUBTRACT, 0x800B);

    // Framebuffer
    ADD_GL_CONST(FRAMEBUFFER, 0x8D40);
    ADD_GL_CONST(RENDERBUFFER, 0x8D41);
    ADD_GL_CONST(COLOR_ATTACHMENT0, 0x8CE0);
    ADD_GL_CONST(DEPTH_ATTACHMENT, 0x8D00);
    ADD_GL_CONST(STENCIL_ATTACHMENT, 0x8D20);
    ADD_GL_CONST(DEPTH_STENCIL_ATTACHMENT, 0x821A);
    ADD_GL_CONST(FRAMEBUFFER_COMPLETE, 0x8CD5);
    ADD_GL_CONST(DEPTH_COMPONENT16, 0x81A5);
    ADD_GL_CONST(STENCIL_INDEX8, 0x8D48);
    ADD_GL_CONST(DEPTH_STENCIL, 0x84F9);

    // Misc
    ADD_GL_CONST(MAX_TEXTURE_SIZE, 0x0D33);
    ADD_GL_CONST(MAX_TEXTURE_IMAGE_UNITS, 0x8872);
    ADD_GL_CONST(MAX_COMBINED_TEXTURE_IMAGE_UNITS, 0x8B4D);
    ADD_GL_CONST(MAX_VERTEX_TEXTURE_IMAGE_UNITS, 0x8B4C);
    ADD_GL_CONST(MAX_VERTEX_ATTRIBS, 0x8869);
    ADD_GL_CONST(MAX_RENDERBUFFER_SIZE, 0x84E8);
    ADD_GL_CONST(RENDERER, 0x1F01);
    ADD_GL_CONST(VENDOR, 0x1F00);
    ADD_GL_CONST(VERSION, 0x1F02);
    ADD_GL_CONST(SHADING_LANGUAGE_VERSION, 0x8B8C);
    ADD_GL_CONST(NO_ERROR, 0);
    ADD_GL_CONST(UNPACK_ALIGNMENT, 0x0CF5);
    ADD_GL_CONST(PACK_ALIGNMENT, 0x0D05);
    ADD_GL_CONST(UNPACK_FLIP_Y_WEBGL, 0x9240);
    ADD_GL_CONST(UNPACK_PREMULTIPLY_ALPHA_WEBGL, 0x9241);
    ADD_GL_CONST(UNPACK_COLORSPACE_CONVERSION_WEBGL, 0x9243);

    // Depth functions
    ADD_GL_CONST(NEVER, 0x0200);
    ADD_GL_CONST(LESS, 0x0201);
    ADD_GL_CONST(EQUAL, 0x0202);
    ADD_GL_CONST(LEQUAL, 0x0203);
    ADD_GL_CONST(GREATER, 0x0204);
    ADD_GL_CONST(NOTEQUAL, 0x0205);
    ADD_GL_CONST(GEQUAL, 0x0206);
    ADD_GL_CONST(ALWAYS, 0x0207);

    // Face culling
    ADD_GL_CONST(FRONT, 0x0404);
    ADD_GL_CONST(BACK, 0x0405);
    ADD_GL_CONST(FRONT_AND_BACK, 0x0408);
    ADD_GL_CONST(CW, 0x0900);
    ADD_GL_CONST(CCW, 0x0901);

    // Boolean
    ADD_GL_CONST(TRUE, 1);
    ADD_GL_CONST(FALSE, 0);

    // Additional constants Phaser uses
    ADD_GL_CONST(CONSTANT_COLOR, 0x8001);
    ADD_GL_CONST(ONE_MINUS_CONSTANT_COLOR, 0x8002);
    ADD_GL_CONST(CONSTANT_ALPHA, 0x8003);
    ADD_GL_CONST(ONE_MINUS_CONSTANT_ALPHA, 0x8004);
    ADD_GL_CONST(BLEND_COLOR, 0x8005);

    // Pixel storage
    ADD_GL_CONST(UNPACK_ROW_LENGTH, 0x0CF2);

    // Shader precision
    ADD_GL_CONST(LOW_FLOAT, 0x8DF0);
    ADD_GL_CONST(MEDIUM_FLOAT, 0x8DF1);
    ADD_GL_CONST(HIGH_FLOAT, 0x8DF2);
    ADD_GL_CONST(LOW_INT, 0x8DF3);
    ADD_GL_CONST(MEDIUM_INT, 0x8DF4);
    ADD_GL_CONST(HIGH_INT, 0x8DF5);

    // Additional buffer info
    ADD_GL_CONST(BUFFER_SIZE, 0x8764);
    ADD_GL_CONST(BUFFER_USAGE, 0x8765);

    // Vertex attrib
    ADD_GL_CONST(CURRENT_VERTEX_ATTRIB, 0x8626);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_ENABLED, 0x8622);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_SIZE, 0x8623);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_STRIDE, 0x8624);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_TYPE, 0x8625);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_NORMALIZED, 0x886A);
    ADD_GL_CONST(VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 0x889F);

    // Renderbuffer info
    ADD_GL_CONST(RENDERBUFFER_WIDTH, 0x8D42);
    ADD_GL_CONST(RENDERBUFFER_HEIGHT, 0x8D43);
    ADD_GL_CONST(RENDERBUFFER_INTERNAL_FORMAT, 0x8D44);
    ADD_GL_CONST(RENDERBUFFER_RED_SIZE, 0x8D50);
    ADD_GL_CONST(RENDERBUFFER_GREEN_SIZE, 0x8D51);
    ADD_GL_CONST(RENDERBUFFER_BLUE_SIZE, 0x8D52);
    ADD_GL_CONST(RENDERBUFFER_ALPHA_SIZE, 0x8D53);
    ADD_GL_CONST(RENDERBUFFER_DEPTH_SIZE, 0x8D54);
    ADD_GL_CONST(RENDERBUFFER_STENCIL_SIZE, 0x8D55);

    // Framebuffer info
    ADD_GL_CONST(FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, 0x8CD0);
    ADD_GL_CONST(FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, 0x8CD1);
    ADD_GL_CONST(FRAMEBUFFER_ATTACHMENT_TEXTURE_LEVEL, 0x8CD2);
    ADD_GL_CONST(FRAMEBUFFER_ATTACHMENT_TEXTURE_CUBE_MAP_FACE, 0x8CD3);
    ADD_GL_CONST(NONE, 0);

    // Hints
    ADD_GL_CONST(DONT_CARE, 0x1100);
    ADD_GL_CONST(FASTEST, 0x1101);
    ADD_GL_CONST(NICEST, 0x1102);
    ADD_GL_CONST(GENERATE_MIPMAP_HINT, 0x8192);

    // Misc get params
    ADD_GL_CONST(BLEND_EQUATION, 0x8009);
    ADD_GL_CONST(BLEND_EQUATION_RGB, 0x8009);
    ADD_GL_CONST(BLEND_EQUATION_ALPHA, 0x883D);
    ADD_GL_CONST(BLEND_DST_RGB, 0x80C8);
    ADD_GL_CONST(BLEND_SRC_RGB, 0x80C9);
    ADD_GL_CONST(BLEND_DST_ALPHA, 0x80CA);
    ADD_GL_CONST(BLEND_SRC_ALPHA, 0x80CB);

    ADD_GL_CONST(DEPTH_COMPONENT, 0x1902);
    ADD_GL_CONST(UNSIGNED_SHORT_4_4_4_4, 0x8033);
    ADD_GL_CONST(UNSIGNED_SHORT_5_5_5_1, 0x8034);
    ADD_GL_CONST(UNSIGNED_SHORT_5_6_5, 0x8363);
    ADD_GL_CONST(DEPTH_COMPONENT16, 0x81A5);

    ADD_GL_CONST(POLYGON_OFFSET_FILL, 0x8037);
    ADD_GL_CONST(SAMPLE_ALPHA_TO_COVERAGE, 0x809E);
    ADD_GL_CONST(SAMPLE_COVERAGE, 0x80A0);
    ADD_GL_CONST(DITHER, 0x0BD0);

    // Store and expose
    g_engine.webgl_ctx_obj = gl;
    g_object_ref(gl);

    // Also add canvas reference to context
    jsc_value_object_set_property(gl, "canvas", g_engine.canvas_obj ? g_engine.canvas_obj : jsc_value_new_null(ctx));

    // drawingBufferWidth/Height
    jsc_value_object_set_property(gl, "drawingBufferWidth",
        jsc_value_new_number(ctx, g_engine.screen_w));
    jsc_value_object_set_property(gl, "drawingBufferHeight",
        jsc_value_new_number(ctx, g_engine.screen_h));

    g_object_unref(gl);
}
