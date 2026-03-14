// dom_bridge.cpp - Bridge between JSC DOM API and litehtml
// Provides a real DOM tree with CSS selector matching and Cairo-based visual
// rendering for Angular/React/Vue/vanilla web apps.

#include "dom_bridge.h"
#include "engine.h"

#include <litehtml.h>
#include <cairo.h>
#include <map>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

// C++ needs explicit cast from gpointer to JSCValue*
#define JSC_ARG(args, i) ((JSCValue*)g_ptr_array_index(args, i))

using namespace litehtml;

// ---------------------------------------------------------------------------
// Element ID tracking
// ---------------------------------------------------------------------------
static int g_next_el_id = 1;
static std::map<int, element::ptr> g_elements;
static std::map<element*, int> g_element_ids;
static document::ptr g_doc;

static int register_element(element::ptr el) {
    if (!el) return 0;
    auto it = g_element_ids.find(el.get());
    if (it != g_element_ids.end()) return it->second;
    int id = g_next_el_id++;
    g_elements[id] = el;
    g_element_ids[el.get()] = id;
    return id;
}

static element::ptr get_element(int id) {
    auto it = g_elements.find(id);
    return (it != g_elements.end()) ? it->second : nullptr;
}

// Register all elements in a subtree recursively
static void register_tree(element::ptr el) {
    if (!el) return;
    register_element(el);
    for (auto& child : el->children()) {
        register_tree(child);
    }
}

// ---------------------------------------------------------------------------
// Font tracking for Cairo rendering
// ---------------------------------------------------------------------------
struct FontInfo {
    std::string family;
    float size;
    bool bold;
    bool italic;
    float ascent;
    float descent;
    float height;
};

static std::map<uint_ptr, FontInfo> g_fonts;
static uint_ptr g_next_font = 1;

// ---------------------------------------------------------------------------
// HTML overlay rendering state
// ---------------------------------------------------------------------------
static cairo_surface_t* g_html_surface = nullptr;
static cairo_t* g_html_cr = nullptr;
static GLuint g_html_texture = 0;
static int g_html_tex_w = 0, g_html_tex_h = 0;
static bool g_html_dirty = true;

// ---------------------------------------------------------------------------
// TinyPhaserContainer - litehtml::document_container with Cairo rendering
// ---------------------------------------------------------------------------
class TinyPhaserContainer : public document_container {
public:
    std::string base_url;

    uint_ptr create_font(const font_description& descr, const document* doc, font_metrics* fm) override {
        FontInfo fi;
        fi.family = "sans-serif";
        // Map web font families to Cairo-friendly names
        if (!descr.family.empty()) {
            std::string fam = descr.family;
            // Strip quotes
            if (fam.size() >= 2 && (fam[0] == '\'' || fam[0] == '"')) {
                fam = fam.substr(1, fam.size() - 2);
            }
            if (fam == "monospace" || fam == "Courier New" || fam == "Consolas") {
                fi.family = "monospace";
            } else if (fam == "serif" || fam == "Times New Roman" || fam == "Georgia") {
                fi.family = "serif";
            } else {
                fi.family = "sans-serif";
            }
        }
        fi.size = descr.size > 0 ? descr.size : 16.0f;
        fi.bold = descr.weight >= 700;
        fi.italic = descr.style == font_style_italic;

        // Get real metrics from Cairo
        if (g_html_cr) {
            cairo_select_font_face(g_html_cr, fi.family.c_str(),
                fi.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
                fi.bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
            cairo_set_font_size(g_html_cr, fi.size);
            cairo_font_extents_t ext;
            cairo_font_extents(g_html_cr, &ext);
            fi.ascent = ext.ascent;
            fi.descent = ext.descent;
            fi.height = ext.height;
        } else {
            fi.ascent = fi.size * 0.8f;
            fi.descent = fi.size * 0.2f;
            fi.height = fi.size * 1.2f;
        }

        if (fm) {
            fm->font_size = fi.size;
            fm->height = (int)fi.height;
            fm->ascent = (int)fi.ascent;
            fm->descent = (int)fi.descent;
            fm->x_height = (int)(fi.size * 0.5f);
            fm->ch_width = (int)(fi.size * 0.6f);
            fm->draw_spaces = true;
            fm->sub_shift = (int)(fi.size * 0.3f);
            fm->super_shift = (int)(fi.size * 0.4f);
        }

        uint_ptr id = g_next_font++;
        g_fonts[id] = fi;
        return id;
    }

    void delete_font(uint_ptr hFont) override {
        g_fonts.erase(hFont);
    }

    pixel_t text_width(const char* text, uint_ptr hFont) override {
        if (!text || !g_html_cr) return 0;
        auto it = g_fonts.find(hFont);
        if (it == g_fonts.end()) return (pixel_t)(strlen(text) * 8);

        const FontInfo& fi = it->second;
        cairo_select_font_face(g_html_cr, fi.family.c_str(),
            fi.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
            fi.bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(g_html_cr, fi.size);
        cairo_text_extents_t ext;
        cairo_text_extents(g_html_cr, text, &ext);
        return (pixel_t)(ext.x_advance);
    }

    void draw_text(uint_ptr hdc, const char* text, uint_ptr hFont,
                   web_color color, const position& pos) override {
        if (!text || !hdc) return;
        cairo_t* cr = (cairo_t*)hdc;
        auto it = g_fonts.find(hFont);
        if (it == g_fonts.end()) return;

        const FontInfo& fi = it->second;
        cairo_select_font_face(cr, fi.family.c_str(),
            fi.italic ? CAIRO_FONT_SLANT_ITALIC : CAIRO_FONT_SLANT_NORMAL,
            fi.bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, fi.size);
        cairo_set_source_rgba(cr, color.red / 255.0, color.green / 255.0,
                              color.blue / 255.0, color.alpha / 255.0);
        cairo_move_to(cr, pos.x, pos.y + fi.ascent);
        cairo_show_text(cr, text);
    }

    pixel_t pt_to_px(float pt) const override {
        return (pixel_t)(pt * 96.0f / 72.0f);
    }

    pixel_t get_default_font_size() const override { return 16; }
    const char* get_default_font_name() const override { return "sans-serif"; }

    void draw_list_marker(uint_ptr hdc, const list_marker& marker) override {
        if (!hdc) return;
        cairo_t* cr = (cairo_t*)hdc;
        cairo_set_source_rgba(cr, marker.color.red / 255.0, marker.color.green / 255.0,
                              marker.color.blue / 255.0, marker.color.alpha / 255.0);
        cairo_arc(cr, marker.pos.x + marker.pos.width / 2.0,
                  marker.pos.y + marker.pos.height / 2.0,
                  marker.pos.width / 3.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    void load_image(const char* src, const char* baseurl, bool redraw_on_ready) override {}
    void get_image_size(const char* src, const char* baseurl, size& sz) override {
        sz.width = 0; sz.height = 0;
    }
    void draw_image(uint_ptr hdc, const background_layer& layer,
                    const std::string& url, const std::string& base_url) override {}

    void draw_solid_fill(uint_ptr hdc, const background_layer& layer,
                         const web_color& color) override {
        if (!hdc || color.alpha == 0) return;
        cairo_t* cr = (cairo_t*)hdc;
        cairo_set_source_rgba(cr, color.red / 255.0, color.green / 255.0,
                              color.blue / 255.0, color.alpha / 255.0);

        auto& box = layer.border_box;
        apply_border_radius(cr, box, layer.border_radius);
        cairo_fill(cr);
    }

    void draw_linear_gradient(uint_ptr hdc, const background_layer& layer,
                              const background_layer::linear_gradient& gradient) override {
        if (!hdc || gradient.color_points.empty()) return;
        cairo_t* cr = (cairo_t*)hdc;
        auto& box = layer.border_box;

        // litehtml provides start/end points directly
        cairo_pattern_t* pat = cairo_pattern_create_linear(
            box.x, box.y, box.x + box.width, box.y + box.height);

        for (auto& cp : gradient.color_points) {
            cairo_pattern_add_color_stop_rgba(pat, cp.offset,
                cp.color.red / 255.0, cp.color.green / 255.0,
                cp.color.blue / 255.0, cp.color.alpha / 255.0);
        }

        cairo_set_source(cr, pat);
        apply_border_radius(cr, box, layer.border_radius);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    void draw_radial_gradient(uint_ptr hdc, const background_layer& layer,
                              const background_layer::radial_gradient& gradient) override {
        if (!hdc || gradient.color_points.empty()) return;
        cairo_t* cr = (cairo_t*)hdc;
        auto& box = layer.border_box;

        double cx = box.x + gradient.position.x;
        double cy = box.y + gradient.position.y;
        double r = std::max(box.width, box.height) / 2.0;

        cairo_pattern_t* pat = cairo_pattern_create_radial(cx, cy, 0, cx, cy, r);
        for (auto& cp : gradient.color_points) {
            cairo_pattern_add_color_stop_rgba(pat, cp.offset,
                cp.color.red / 255.0, cp.color.green / 255.0,
                cp.color.blue / 255.0, cp.color.alpha / 255.0);
        }

        cairo_set_source(cr, pat);
        apply_border_radius(cr, box, layer.border_radius);
        cairo_fill(cr);
        cairo_pattern_destroy(pat);
    }

    void draw_conic_gradient(uint_ptr hdc, const background_layer& layer,
                             const background_layer::conic_gradient& gradient) override {
        // Approximate as solid with first color
        if (!hdc || gradient.color_points.empty()) return;
        auto& c = gradient.color_points[0].color;
        web_color wc;
        wc.red = c.red; wc.green = c.green; wc.blue = c.blue; wc.alpha = c.alpha;
        draw_solid_fill(hdc, layer, wc);
    }

    void draw_borders(uint_ptr hdc, const borders& b,
                      const position& draw_pos, bool root) override {
        if (!hdc) return;
        cairo_t* cr = (cairo_t*)hdc;

        // Draw each border side
        auto draw_side = [&](const border& side, double x1, double y1, double x2, double y2) {
            if (side.width == 0 || side.style == border_style_none ||
                side.style == border_style_hidden) return;
            cairo_set_source_rgba(cr, side.color.red / 255.0, side.color.green / 255.0,
                                  side.color.blue / 255.0, side.color.alpha / 255.0);
            cairo_set_line_width(cr, side.width);
            cairo_move_to(cr, x1, y1);
            cairo_line_to(cr, x2, y2);
            cairo_stroke(cr);
        };

        double x = draw_pos.x, y = draw_pos.y;
        double w = draw_pos.width, h = draw_pos.height;

        draw_side(b.top,    x, y,     x + w, y);
        draw_side(b.bottom, x, y + h, x + w, y + h);
        draw_side(b.left,   x, y,     x,     y + h);
        draw_side(b.right,  x + w, y, x + w, y + h);
    }

    void set_caption(const char* caption) override {
        if (g_engine.window) SDL_SetWindowTitle(g_engine.window, caption);
    }

    void set_base_url(const char* url) override {
        base_url = url ? url : "";
    }

    void link(const std::shared_ptr<litehtml::document>& doc, const element::ptr& el) override {}
    void on_anchor_click(const char* url, const element::ptr& el) override {}
    void on_mouse_event(const element::ptr& el, mouse_event event) override {}
    void set_cursor(const char* cursor) override {}

    void transform_text(litehtml::string& text, text_transform tt) override {
        if (tt == text_transform_capitalize && !text.empty()) {
            text[0] = toupper(text[0]);
        } else if (tt == text_transform_uppercase) {
            for (auto& c : text) c = toupper(c);
        } else if (tt == text_transform_lowercase) {
            for (auto& c : text) c = tolower(c);
        }
    }

    void import_css(litehtml::string& text, const litehtml::string& url,
                    litehtml::string& baseurl) override {
        if (g_engine.game_dir && !url.empty()) {
            char path[2048];
            snprintf(path, sizeof(path), "%s/%s", g_engine.game_dir, url.c_str());
            size_t len;
            char* data = engine_read_file(path, &len);
            if (data) {
                text = data;
                free(data);
            }
        }
    }

    void set_clip(const position& pos, const border_radiuses& bdr_radius) override {
        if (!g_html_cr) return;
        cairo_save(g_html_cr);
        cairo_rectangle(g_html_cr, pos.x, pos.y, pos.width, pos.height);
        cairo_clip(g_html_cr);
    }

    void del_clip() override {
        if (!g_html_cr) return;
        cairo_restore(g_html_cr);
    }

    void get_viewport(position& viewport) const override {
        viewport.x = 0;
        viewport.y = 0;
        viewport.width = g_engine.render_w ? g_engine.render_w : g_engine.screen_w;
        viewport.height = g_engine.render_h ? g_engine.render_h : g_engine.screen_h;
    }

    element::ptr create_element(const char* tag_name, const string_map& attributes,
                                const std::shared_ptr<litehtml::document>& doc) override {
        return nullptr; // let litehtml create default elements
    }

    void get_media_features(media_features& media) const override {
        media.type = media_type_screen;
        int w = g_engine.render_w ? g_engine.render_w : g_engine.screen_w;
        int h = g_engine.render_h ? g_engine.render_h : g_engine.screen_h;
        media.width = w;
        media.height = h;
        media.device_width = w;
        media.device_height = h;
        media.color = 8;
        media.color_index = 256;
        media.monochrome = 0;
        media.resolution = 96;
    }

    void get_language(litehtml::string& language, litehtml::string& culture) const override {
        language = "en";
        culture = "";
    }

private:
    // Helper: create a rounded rectangle path for border-radius
    void apply_border_radius(cairo_t* cr, const position& box, const border_radiuses& radius) {
        double x = box.x, y = box.y, w = box.width, h = box.height;

        int tl = radius.top_left_x;
        int tr = radius.top_right_x;
        int br = radius.bottom_right_x;
        int bl = radius.bottom_left_x;

        if (tl == 0 && tr == 0 && br == 0 && bl == 0) {
            cairo_rectangle(cr, x, y, w, h);
            return;
        }

        cairo_new_path(cr);
        cairo_move_to(cr, x + tl, y);
        cairo_line_to(cr, x + w - tr, y);
        if (tr > 0) cairo_arc(cr, x + w - tr, y + tr, tr, -M_PI / 2, 0);
        cairo_line_to(cr, x + w, y + h - br);
        if (br > 0) cairo_arc(cr, x + w - br, y + h - br, br, 0, M_PI / 2);
        cairo_line_to(cr, x + bl, y + h);
        if (bl > 0) cairo_arc(cr, x + bl, y + h - bl, bl, M_PI / 2, M_PI);
        cairo_line_to(cr, x, y + tl);
        if (tl > 0) cairo_arc(cr, x + tl, y + tl, tl, M_PI, 3 * M_PI / 2);
        cairo_close_path(cr);
    }
};

static TinyPhaserContainer* g_container = nullptr;

// ---------------------------------------------------------------------------
// HTML Overlay Rendering
// ---------------------------------------------------------------------------

static void ensure_html_surface(int w, int h) {
    if (g_html_surface && g_html_tex_w == w && g_html_tex_h == h) return;

    if (g_html_cr) cairo_destroy(g_html_cr);
    if (g_html_surface) cairo_surface_destroy(g_html_surface);

    g_html_surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    g_html_cr = cairo_create(g_html_surface);
    g_html_tex_w = w;
    g_html_tex_h = h;

    if (!g_html_texture) {
        glGenTextures(1, &g_html_texture);
    }
    glBindTexture(GL_TEXTURE_2D, g_html_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, NULL);
}

// Upload Cairo surface to GL texture (ARGB premultiplied → BGRA works directly)
static void upload_html_texture() {
    if (!g_html_surface || !g_html_texture) return;

    cairo_surface_flush(g_html_surface);
    unsigned char* data = cairo_image_surface_get_data(g_html_surface);
    int stride = cairo_image_surface_get_stride(g_html_surface);

    glBindTexture(GL_TEXTURE_2D, g_html_texture);
    if (stride == g_html_tex_w * 4) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_html_tex_w, g_html_tex_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, data);
    } else {
        // Row-by-row upload for non-standard stride
        glPixelStorei(GL_UNPACK_ROW_LENGTH, stride / 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_html_tex_w, g_html_tex_h,
                        GL_BGRA, GL_UNSIGNED_BYTE, data);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
}

// Simple fullscreen textured quad shader (compiled once)
static GLuint g_html_shader = 0;
static GLuint g_html_vbo = 0;

static const char* html_vs_src =
    "#version 100\n"
    "attribute vec2 aPos;\n"
    "attribute vec2 aUV;\n"
    "varying vec2 vUV;\n"
    "void main() {\n"
    "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
    "  vUV = aUV;\n"
    "}\n";

static const char* html_fs_src =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 vUV;\n"
    "uniform sampler2D uTex;\n"
    "void main() {\n"
    "  vec4 c = texture2D(uTex, vUV);\n"
    // Cairo outputs premultiplied alpha — un-premultiply for GL blending
    "  if (c.a > 0.001) c.rgb /= c.a;\n"
    "  gl_FragColor = c;\n"
    "}\n";

static void init_html_renderer() {
    if (g_html_shader) return;

    // Compile shaders
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &html_vs_src, NULL);
    glCompileShader(vs);

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &html_fs_src, NULL);
    glCompileShader(fs);

    g_html_shader = glCreateProgram();
    glAttachShader(g_html_shader, vs);
    glAttachShader(g_html_shader, fs);
    glLinkProgram(g_html_shader);
    glDeleteShader(vs);
    glDeleteShader(fs);

    // Fullscreen quad: position (xy) + UV
    float quad[] = {
        -1, -1,  0, 1,
         1, -1,  1, 1,
        -1,  1,  0, 0,
         1,  1,  1, 0,
    };

    glGenBuffers(1, &g_html_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_html_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
}

static void draw_html_quad() {
    if (!g_html_texture || !g_html_shader) return;

    GLint old_prog;
    glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);

    glUseProgram(g_html_shader);

    GLint posLoc = glGetAttribLocation(g_html_shader, "aPos");
    GLint uvLoc = glGetAttribLocation(g_html_shader, "aUV");

    glBindBuffer(GL_ARRAY_BUFFER, g_html_vbo);
    glEnableVertexAttribArray(posLoc);
    glVertexAttribPointer(posLoc, 2, GL_FLOAT, GL_FALSE, 16, (void*)0);
    glEnableVertexAttribArray(uvLoc);
    glVertexAttribPointer(uvLoc, 2, GL_FLOAT, GL_FALSE, 16, (void*)8);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g_html_texture);
    glUniform1i(glGetUniformLocation(g_html_shader, "uTex"), 0);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    glDisableVertexAttribArray(posLoc);
    glDisableVertexAttribArray(uvLoc);
    glUseProgram(old_prog);
}

// Public API: render HTML overlay and draw it
extern "C" void dom_bridge_render(void) {
    if (!g_doc) return;

    int w = g_engine.render_w ? g_engine.render_w : g_engine.screen_w;
    int h = g_engine.render_h ? g_engine.render_h : g_engine.screen_h;

    init_html_renderer();
    ensure_html_surface(w, h);

    // Re-layout if needed
    g_doc->render(w);

    // Clear surface (transparent)
    cairo_set_operator(g_html_cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(g_html_cr, 0, 0, 0, 0);
    cairo_paint(g_html_cr);
    cairo_set_operator(g_html_cr, CAIRO_OPERATOR_OVER);

    // Render litehtml — passes cairo_t* as hdc to all draw callbacks
    position clip(0, 0, w, h);
    g_doc->draw((uint_ptr)g_html_cr, 0, 0, &clip);

    // Upload to GL texture and draw
    upload_html_texture();
    draw_html_quad();

    g_html_dirty = false;
}

// ---------------------------------------------------------------------------
// Native JSC functions (unchanged from Phase 1)
// ---------------------------------------------------------------------------

// __domCreateElement(tag) → element_id
static JSCValue* native_dom_create_element(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (!g_doc || args->len < 1) return jsc_value_new_number(ctx, 0);

    char* tag = jsc_value_to_string(JSC_ARG(args, 0));
    string_map attrs;
    auto el = g_doc->create_element(tag, attrs);
    g_free(tag);

    if (!el) return jsc_value_new_number(ctx, 0);
    g_html_dirty = true;
    return jsc_value_new_number(ctx, register_element(el));
}

// __domCreateTextNode(text) → element_id
static JSCValue* native_dom_create_text_node(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (!g_doc || args->len < 1) return jsc_value_new_number(ctx, 0);

    char* text = jsc_value_to_string(JSC_ARG(args, 0));
    string_map attrs;
    auto el = g_doc->create_element("", attrs);
    if (el) {
        el->set_data(text);
    }
    g_free(text);

    if (!el) return jsc_value_new_number(ctx, 0);
    return jsc_value_new_number(ctx, register_element(el));
}

// __domAppendChild(parentId, childId) → success
static JSCValue* native_dom_append_child(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_boolean(ctx, FALSE);

    int parent_id = jsc_value_to_int32(JSC_ARG(args, 0));
    int child_id = jsc_value_to_int32(JSC_ARG(args, 1));

    auto parent = get_element(parent_id);
    auto child = get_element(child_id);
    if (!parent || !child) return jsc_value_new_boolean(ctx, FALSE);

    auto old_parent = child->parent();
    if (old_parent) {
        old_parent->removeChild(child);
    }

    bool ok = parent->appendChild(child);
    g_html_dirty = true;
    return jsc_value_new_boolean(ctx, ok);
}

// __domRemoveChild(parentId, childId) → success
static JSCValue* native_dom_remove_child(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_boolean(ctx, FALSE);

    int parent_id = jsc_value_to_int32(JSC_ARG(args, 0));
    int child_id = jsc_value_to_int32(JSC_ARG(args, 1));

    auto parent = get_element(parent_id);
    auto child = get_element(child_id);
    if (!parent || !child) return jsc_value_new_boolean(ctx, FALSE);

    bool ok = parent->removeChild(child);
    g_html_dirty = true;
    return jsc_value_new_boolean(ctx, ok);
}

// __domInsertBefore(parentId, newChildId, refChildId) → success
static JSCValue* native_dom_insert_before(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 3) return jsc_value_new_boolean(ctx, FALSE);

    int parent_id = jsc_value_to_int32(JSC_ARG(args, 0));
    int new_id = jsc_value_to_int32(JSC_ARG(args, 1));
    int ref_id = jsc_value_to_int32(JSC_ARG(args, 2));

    auto parent = get_element(parent_id);
    auto new_child = get_element(new_id);
    if (!parent || !new_child) return jsc_value_new_boolean(ctx, FALSE);

    auto old_parent = new_child->parent();
    if (old_parent) {
        old_parent->removeChild(new_child);
    }

    if (ref_id == 0) {
        g_html_dirty = true;
        return jsc_value_new_boolean(ctx, parent->appendChild(new_child));
    }

    auto ref_child = get_element(ref_id);
    if (!ref_child) {
        g_html_dirty = true;
        return jsc_value_new_boolean(ctx, parent->appendChild(new_child));
    }

    auto& children = parent->children();
    std::vector<element::ptr> after;
    bool found = false;
    for (auto it = children.begin(); it != children.end(); ) {
        if (found) {
            after.push_back(*it);
            it = const_cast<std::list<element::ptr>&>(children).erase(it);
        } else if (it->get() == ref_child.get()) {
            found = true;
            after.push_back(*it);
            it = const_cast<std::list<element::ptr>&>(children).erase(it);
        } else {
            ++it;
        }
    }

    parent->appendChild(new_child);
    for (auto& el : after) {
        parent->appendChild(el);
    }

    g_html_dirty = true;
    return jsc_value_new_boolean(ctx, TRUE);
}

// __domQuerySelector(contextId, selector) → element_id or 0
static JSCValue* native_dom_query_selector(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_number(ctx, 0);

    int context_id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* selector = jsc_value_to_string(JSC_ARG(args, 1));

    element::ptr context_el;
    if (context_id == 0 && g_doc) {
        context_el = g_doc->root();
    } else {
        context_el = get_element(context_id);
    }

    int result_id = 0;
    if (context_el) {
        auto found = context_el->select_one(selector);
        if (found) {
            result_id = register_element(found);
        }
    }

    g_free(selector);
    return jsc_value_new_number(ctx, result_id);
}

// __domQuerySelectorAll(contextId, selector) → [element_ids]
static JSCValue* native_dom_query_selector_all(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_array(ctx, G_TYPE_NONE);

    int context_id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* selector = jsc_value_to_string(JSC_ARG(args, 1));

    element::ptr context_el;
    if (context_id == 0 && g_doc) {
        context_el = g_doc->root();
    } else {
        context_el = get_element(context_id);
    }

    JSCValue* arr = jsc_context_evaluate(ctx, "[]", -1);

    if (context_el) {
        auto results = context_el->select_all(std::string(selector));
        for (auto& el : results) {
            int id = register_element(el);
            JSCValue* id_val = jsc_value_new_number(ctx, id);
            jsc_value_object_invoke_method(arr, "push", JSC_TYPE_VALUE, id_val, G_TYPE_NONE);
            g_object_unref(id_val);
        }
    }

    g_free(selector);
    return arr;
}

// __domGetAttr(id, name) → string or null
static JSCValue* native_dom_get_attr(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_null(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* name = jsc_value_to_string(JSC_ARG(args, 1));

    auto el = get_element(id);
    JSCValue* result;
    if (el) {
        const char* val = el->get_attr(name);
        result = val ? jsc_value_new_string(ctx, val) : jsc_value_new_null(ctx);
    } else {
        result = jsc_value_new_null(ctx);
    }

    g_free(name);
    return result;
}

// __domSetAttr(id, name, value)
static JSCValue* native_dom_set_attr(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 3) return jsc_value_new_undefined(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* name = jsc_value_to_string(JSC_ARG(args, 1));
    char* value = jsc_value_to_string(JSC_ARG(args, 2));

    auto el = get_element(id);
    if (el) {
        el->set_attr(name, value);
        g_html_dirty = true;
    }

    g_free(name);
    g_free(value);
    return jsc_value_new_undefined(ctx);
}

// __domRemoveAttr(id, name)
static JSCValue* native_dom_remove_attr(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_undefined(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* name = jsc_value_to_string(JSC_ARG(args, 1));

    auto el = get_element(id);
    if (el) {
        el->set_attr(name, nullptr);
        g_html_dirty = true;
    }

    g_free(name);
    return jsc_value_new_undefined(ctx);
}

// __domGetTagName(id) → string
static JSCValue* native_dom_get_tag_name(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_string(ctx, "");

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    if (el) {
        const char* tag = el->get_tagName();
        return jsc_value_new_string(ctx, tag ? tag : "");
    }
    return jsc_value_new_string(ctx, "");
}

// __domGetParentId(id) → parent_id or 0
static JSCValue* native_dom_get_parent_id(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_number(ctx, 0);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    if (el) {
        auto parent = el->parent();
        if (parent) {
            return jsc_value_new_number(ctx, register_element(parent));
        }
    }
    return jsc_value_new_number(ctx, 0);
}

// __domGetChildIds(id) → [child_ids]
static JSCValue* native_dom_get_child_ids(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_array(ctx, G_TYPE_NONE);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    JSCValue* arr = jsc_context_evaluate(ctx, "[]", -1);
    if (el) {
        for (auto& child : el->children()) {
            int cid = register_element(child);
            JSCValue* v = jsc_value_new_number(ctx, cid);
            jsc_value_object_invoke_method(arr, "push", JSC_TYPE_VALUE, v, G_TYPE_NONE);
            g_object_unref(v);
        }
    }
    return arr;
}

// __domGetText(id) → string
static JSCValue* native_dom_get_text(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_string(ctx, "");

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    if (el) {
        litehtml::string text;
        el->get_text(text);
        return jsc_value_new_string(ctx, text.c_str());
    }
    return jsc_value_new_string(ctx, "");
}

// __domSetText(id, text)
static JSCValue* native_dom_set_text(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 2) return jsc_value_new_undefined(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* text = jsc_value_to_string(JSC_ARG(args, 1));

    auto el = get_element(id);
    if (el) {
        el->set_data(text);
        g_html_dirty = true;
    }

    g_free(text);
    return jsc_value_new_undefined(ctx);
}

// __domSetInnerHTML(id, html)
static JSCValue* native_dom_set_inner_html(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (!g_doc || args->len < 2) return jsc_value_new_undefined(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* html = jsc_value_to_string(JSC_ARG(args, 1));

    auto el = get_element(id);
    if (el) {
        g_doc->append_children_from_string(*el, html, true);
        for (auto& child : el->children()) {
            register_tree(child);
        }
        g_html_dirty = true;
    }

    g_free(html);
    return jsc_value_new_undefined(ctx);
}

// __domSetClass(id, className, add)
static JSCValue* native_dom_set_class(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 3) return jsc_value_new_undefined(ctx);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    char* cls = jsc_value_to_string(JSC_ARG(args, 1));
    bool add = jsc_value_to_boolean(JSC_ARG(args, 2));

    auto el = get_element(id);
    if (el) {
        el->set_class(cls, add);
        g_html_dirty = true;
    }

    g_free(cls);
    return jsc_value_new_undefined(ctx);
}

// __domIsText(id) → bool
static JSCValue* native_dom_is_text(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_boolean(ctx, FALSE);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    return jsc_value_new_boolean(ctx, el && el->is_text());
}

// __domIsComment(id) → bool
static JSCValue* native_dom_is_comment(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (args->len < 1) return jsc_value_new_boolean(ctx, FALSE);

    int id = jsc_value_to_int32(JSC_ARG(args, 0));
    auto el = get_element(id);

    return jsc_value_new_boolean(ctx, el && el->is_comment());
}

// __domGetDocRootId() → id of <html> element
static JSCValue* native_dom_get_doc_root_id(GPtrArray* args, gpointer user_data) {
    JSCContext* ctx = jsc_context_get_current();
    if (!g_doc) return jsc_value_new_number(ctx, 0);

    auto root = g_doc->root();
    if (!root) return jsc_value_new_number(ctx, 0);

    for (auto& child : root->children()) {
        const char* tag = child->get_tagName();
        if (tag && strcasecmp(tag, "html") == 0) {
            return jsc_value_new_number(ctx, register_element(child));
        }
    }
    return jsc_value_new_number(ctx, register_element(root));
}

// ---------------------------------------------------------------------------
// Registration
// ---------------------------------------------------------------------------

static void register_fn(JSCContext* ctx, const char* name, GCallback cb) {
    JSCValue* fn = jsc_value_new_function_variadic(ctx, name, cb, NULL, NULL, JSC_TYPE_VALUE);
    jsc_context_set_value(ctx, name, fn);
    g_object_unref(fn);
}

extern "C" void register_dom_bridge_shim(JSCContext* ctx) {
    register_fn(ctx, "__domCreateElement", G_CALLBACK(native_dom_create_element));
    register_fn(ctx, "__domCreateTextNode", G_CALLBACK(native_dom_create_text_node));
    register_fn(ctx, "__domAppendChild", G_CALLBACK(native_dom_append_child));
    register_fn(ctx, "__domRemoveChild", G_CALLBACK(native_dom_remove_child));
    register_fn(ctx, "__domInsertBefore", G_CALLBACK(native_dom_insert_before));
    register_fn(ctx, "__domQuerySelector", G_CALLBACK(native_dom_query_selector));
    register_fn(ctx, "__domQuerySelectorAll", G_CALLBACK(native_dom_query_selector_all));
    register_fn(ctx, "__domGetAttr", G_CALLBACK(native_dom_get_attr));
    register_fn(ctx, "__domSetAttr", G_CALLBACK(native_dom_set_attr));
    register_fn(ctx, "__domRemoveAttr", G_CALLBACK(native_dom_remove_attr));
    register_fn(ctx, "__domGetTagName", G_CALLBACK(native_dom_get_tag_name));
    register_fn(ctx, "__domGetParentId", G_CALLBACK(native_dom_get_parent_id));
    register_fn(ctx, "__domGetChildIds", G_CALLBACK(native_dom_get_child_ids));
    register_fn(ctx, "__domGetText", G_CALLBACK(native_dom_get_text));
    register_fn(ctx, "__domSetText", G_CALLBACK(native_dom_set_text));
    register_fn(ctx, "__domSetInnerHTML", G_CALLBACK(native_dom_set_inner_html));
    register_fn(ctx, "__domSetClass", G_CALLBACK(native_dom_set_class));
    register_fn(ctx, "__domIsText", G_CALLBACK(native_dom_is_text));
    register_fn(ctx, "__domIsComment", G_CALLBACK(native_dom_is_comment));
    register_fn(ctx, "__domGetDocRootId", G_CALLBACK(native_dom_get_doc_root_id));

    printf("[DOM] Bridge registered with Cairo rendering\n");
}

extern "C" void dom_bridge_load_html(JSCContext* ctx, const char* html) {
    if (!html) return;

    if (!g_container) {
        g_container = new TinyPhaserContainer();
    }

    // CRITICAL: Create Cairo surface BEFORE parsing HTML so that
    // create_font() gets real metrics and text_width() works during layout.
    // Without this, text_width() returns 0 and all text overlaps.
    int w = g_engine.render_w ? g_engine.render_w : g_engine.screen_w;
    int h = g_engine.render_h ? g_engine.render_h : g_engine.screen_h;
    if (w <= 0) w = 640;
    if (h <= 0) h = 480;
    ensure_html_surface(w, h);

    g_doc = document::createFromString(html, g_container);

    if (g_doc) {
        g_doc->render(w);
        register_tree(g_doc->root());
        g_html_dirty = true;
        printf("[DOM] Parsed HTML document (%d elements registered)\n", g_next_el_id - 1);
    } else {
        fprintf(stderr, "[DOM] Failed to parse HTML\n");
    }
}

extern "C" void dom_bridge_shutdown(void) {
    g_elements.clear();
    g_element_ids.clear();
    g_fonts.clear();
    g_doc.reset();
    delete g_container;
    g_container = nullptr;
    g_next_el_id = 1;
    g_next_font = 1;

    if (g_html_cr) { cairo_destroy(g_html_cr); g_html_cr = nullptr; }
    if (g_html_surface) { cairo_surface_destroy(g_html_surface); g_html_surface = nullptr; }
    if (g_html_texture) { glDeleteTextures(1, &g_html_texture); g_html_texture = 0; }
    if (g_html_shader) { glDeleteProgram(g_html_shader); g_html_shader = 0; }
    if (g_html_vbo) { glDeleteBuffers(1, &g_html_vbo); g_html_vbo = 0; }
}
