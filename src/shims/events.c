#include "engine.h"

// Transform window coordinates to game coordinates (accounting for FBO letterboxing)
// Clamps to game bounds so clicks in black bars map to the nearest game edge.
static void window_to_game_coords(int wx, int wy, int *gx, int *gy) {
    if (!g_engine.fbo) {
        *gx = wx;
        *gy = wy;
        return;
    }
    float scale_x = (float)g_engine.screen_w / g_engine.render_w;
    float scale_y = (float)g_engine.screen_h / g_engine.render_h;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    int draw_w = (int)(g_engine.render_w * scale);
    int draw_h = (int)(g_engine.render_h * scale);
    int offset_x = (g_engine.screen_w - draw_w) / 2;
    int offset_y = (g_engine.screen_h - draw_h) / 2;

    int x = (int)((wx - offset_x) * (float)g_engine.render_w / draw_w);
    int y = (int)((wy - offset_y) * (float)g_engine.render_h / draw_h);
    // Clamp to game bounds
    if (x < 0) x = 0;
    if (x >= g_engine.render_w) x = g_engine.render_w - 1;
    if (y < 0) y = 0;
    if (y >= g_engine.render_h) y = g_engine.render_h - 1;
    *gx = x;
    *gy = y;
}

// SDL keycode to DOM key string
static const char *sdl_to_dom_key(SDL_Keycode key) {
    switch (key) {
        case SDLK_UP:     return "ArrowUp";
        case SDLK_DOWN:   return "ArrowDown";
        case SDLK_LEFT:   return "ArrowLeft";
        case SDLK_RIGHT:  return "ArrowRight";
        case SDLK_SPACE:  return " ";
        case SDLK_RETURN: return "Enter";
        case SDLK_ESCAPE: return "Escape";
        case SDLK_TAB:    return "Tab";
        case SDLK_BACKSPACE: return "Backspace";
        case SDLK_DELETE: return "Delete";
        case SDLK_LSHIFT: case SDLK_RSHIFT: return "Shift";
        case SDLK_LCTRL:  case SDLK_RCTRL:  return "Control";
        case SDLK_LALT:   case SDLK_RALT:   return "Alt";
        default:
            if (key >= SDLK_a && key <= SDLK_z) {
                static char buf[2];
                buf[0] = (char)key;
                buf[1] = '\0';
                return buf;
            }
            if (key >= SDLK_0 && key <= SDLK_9) {
                static char nbuf[2];
                nbuf[0] = (char)key;
                nbuf[1] = '\0';
                return nbuf;
            }
            return "Unidentified";
    }
}

static const char *sdl_to_dom_code(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP:    return "ArrowUp";
        case SDL_SCANCODE_DOWN:  return "ArrowDown";
        case SDL_SCANCODE_LEFT:  return "ArrowLeft";
        case SDL_SCANCODE_RIGHT: return "ArrowRight";
        case SDL_SCANCODE_SPACE: return "Space";
        case SDL_SCANCODE_RETURN: return "Enter";
        case SDL_SCANCODE_ESCAPE: return "Escape";
        case SDL_SCANCODE_TAB:   return "Tab";
        case SDL_SCANCODE_LSHIFT: return "ShiftLeft";
        case SDL_SCANCODE_RSHIFT: return "ShiftRight";
        case SDL_SCANCODE_LCTRL:  return "ControlLeft";
        case SDL_SCANCODE_RCTRL:  return "ControlRight";
        default:
            if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) {
                static char buf[8];
                snprintf(buf, sizeof(buf), "Key%c", 'A' + (sc - SDL_SCANCODE_A));
                return buf;
            }
            if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_0) {
                static char nbuf[8];
                int digit = (sc == SDL_SCANCODE_0) ? 0 : (sc - SDL_SCANCODE_1 + 1);
                snprintf(nbuf, sizeof(nbuf), "Digit%d", digit);
                return nbuf;
            }
            return "";
    }
}

static int sdl_to_keycode(SDL_Keycode key) {
    // DOM keyCode values for common keys
    if (key >= SDLK_a && key <= SDLK_z) return key - SDLK_a + 65;
    if (key >= SDLK_0 && key <= SDLK_9) return key - SDLK_0 + 48;
    switch (key) {
        case SDLK_UP:     return 38;
        case SDLK_DOWN:   return 40;
        case SDLK_LEFT:   return 37;
        case SDLK_RIGHT:  return 39;
        case SDLK_SPACE:  return 32;
        case SDLK_RETURN: return 13;
        case SDLK_ESCAPE: return 27;
        case SDLK_TAB:    return 9;
        case SDLK_LSHIFT: case SDLK_RSHIFT: return 16;
        case SDLK_LCTRL:  case SDLK_RCTRL:  return 17;
        case SDLK_LALT:   case SDLK_RALT:   return 18;
        default: return 0;
    }
}

static void fire_key_event(const char *type, SDL_KeyboardEvent *key) {
    JSCContext *ctx = g_engine.js_ctx;

    const char *dom_key = sdl_to_dom_key(key->keysym.sym);
    const char *dom_code = sdl_to_dom_code(key->keysym.scancode);
    int keyCode = sdl_to_keycode(key->keysym.sym);

    char js[2048];
    snprintf(js, sizeof(js),
        "(function() {"
        "  var e = { type:'%s', key:'%s', code:'%s', keyCode:%d, which:%d,"
        "    ctrlKey:%s, shiftKey:%s, altKey:%s, metaKey:false,"
        "    repeat:%s, preventDefault:function(){}, stopPropagation:function(){} };"
        "  if (window._eventListeners && window._eventListeners['%s']) {"
        "    window._eventListeners['%s'].slice().forEach(function(cb){ cb(e); });"
        "  }"
        "  if (typeof document !== 'undefined' && document._listeners && document._listeners['%s']) {"
        "    document._listeners['%s'].slice().forEach(function(cb){ cb(e); });"
        "  }"
        "})();",
        type, dom_key, dom_code, keyCode, keyCode,
        (key->keysym.mod & KMOD_CTRL) ? "true" : "false",
        (key->keysym.mod & KMOD_SHIFT) ? "true" : "false",
        (key->keysym.mod & KMOD_ALT) ? "true" : "false",
        key->repeat ? "true" : "false",
        type, type, type, type);

    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    if (r) g_object_unref(r);
}

static void fire_mouse_event(const char *type, int x, int y, int button) {
    JSCContext *ctx = g_engine.js_ctx;

    char js[2048];
    snprintf(js, sizeof(js),
        "(function() {"
        "  var pc = window._primaryCanvas;"
        "  var e = { type:'%s', clientX:%d, clientY:%d, pageX:%d, pageY:%d,"
        "    screenX:%d, screenY:%d, offsetX:%d, offsetY:%d, movementX:0, movementY:0,"
        "    button:%d, buttons:%d,"
        "    pointerId:1, pointerType:'mouse', isPrimary:true, width:1, height:1,"
        "    target: pc || null, currentTarget: pc || null,"
        "    bubbles:true, cancelable:true, composed:true,"
        "    preventDefault:function(){}, stopPropagation:function(){}, stopImmediatePropagation:function(){} };"
        "  if (pc && pc._listeners && pc._listeners['%s']) {"
        "    pc._listeners['%s'].slice().forEach(function(cb){ cb(e); });"
        "  }"
        "  if (window._eventListeners && window._eventListeners['%s']) {"
        "    window._eventListeners['%s'].slice().forEach(function(cb){ cb(e); });"
        "  }"
        "  if (typeof document !== 'undefined' && document._listeners && document._listeners['%s']) {"
        "    document._listeners['%s'].slice().forEach(function(cb){ cb(e); });"
        "  }"
        "})();",
        type, x, y, x, y, x, y, x, y, button, button ? 1 : 0,
        type, type, type, type, type, type);

    JSCValue *r = jsc_context_evaluate(ctx, js, -1);
    if (r) g_object_unref(r);
}

// --- Gamepad/Joystick → Keyboard mapping ---
// Track axis/hat state to generate key down/up transitions
static bool joy_left = false, joy_right = false;
static bool joy_up = false, joy_down = false;

#define JOY_AXIS_THRESHOLD 16000

static void inject_key_event(const char *type, SDL_Keycode sym, SDL_Scancode sc) {
    SDL_KeyboardEvent key = {0};
    key.keysym.sym = sym;
    key.keysym.scancode = sc;
    key.repeat = 0;
    fire_key_event(type, &key);
}

// Map joystick buttons to keyboard keys
static void translate_joy_button(int button, const char *type) {
    switch (button) {
        case 0: // A/South → Space (action/confirm)
            inject_key_event(type, SDLK_SPACE, SDL_SCANCODE_SPACE);
            break;
        case 1: // B/East → Escape (back/cancel)
            inject_key_event(type, SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
            break;
        case 2: // X/West → z
            inject_key_event(type, SDLK_z, SDL_SCANCODE_Z);
            break;
        case 3: // Y/North → x
            inject_key_event(type, SDLK_x, SDL_SCANCODE_X);
            break;
        case 4: // L1 → Shift
            inject_key_event(type, SDLK_LSHIFT, SDL_SCANCODE_LSHIFT);
            break;
        case 5: // R1 → Ctrl
            inject_key_event(type, SDLK_LCTRL, SDL_SCANCODE_LCTRL);
            break;
        case 6: // Select → Tab
            inject_key_event(type, SDLK_TAB, SDL_SCANCODE_TAB);
            break;
        case 7: // Start → Enter
            inject_key_event(type, SDLK_RETURN, SDL_SCANCODE_RETURN);
            break;
    }
}

static void translate_joy_axis(int axis, int value) {
    if (axis == 0) { // X axis
        bool left = value < -JOY_AXIS_THRESHOLD;
        bool right = value > JOY_AXIS_THRESHOLD;
        if (left != joy_left) {
            inject_key_event(left ? "keydown" : "keyup", SDLK_LEFT, SDL_SCANCODE_LEFT);
            joy_left = left;
        }
        if (right != joy_right) {
            inject_key_event(right ? "keydown" : "keyup", SDLK_RIGHT, SDL_SCANCODE_RIGHT);
            joy_right = right;
        }
    } else if (axis == 1) { // Y axis
        bool up = value < -JOY_AXIS_THRESHOLD;
        bool down = value > JOY_AXIS_THRESHOLD;
        if (up != joy_up) {
            inject_key_event(up ? "keydown" : "keyup", SDLK_UP, SDL_SCANCODE_UP);
            joy_up = up;
        }
        if (down != joy_down) {
            inject_key_event(down ? "keydown" : "keyup", SDLK_DOWN, SDL_SCANCODE_DOWN);
            joy_down = down;
        }
    }
}

static void translate_joy_hat(int value) {
    bool up    = (value & SDL_HAT_UP) != 0;
    bool down  = (value & SDL_HAT_DOWN) != 0;
    bool left  = (value & SDL_HAT_LEFT) != 0;
    bool right = (value & SDL_HAT_RIGHT) != 0;

    if (up != joy_up)    { inject_key_event(up ? "keydown" : "keyup", SDLK_UP, SDL_SCANCODE_UP); joy_up = up; }
    if (down != joy_down) { inject_key_event(down ? "keydown" : "keyup", SDLK_DOWN, SDL_SCANCODE_DOWN); joy_down = down; }
    if (left != joy_left) { inject_key_event(left ? "keydown" : "keyup", SDLK_LEFT, SDL_SCANCODE_LEFT); joy_left = left; }
    if (right != joy_right) { inject_key_event(right ? "keydown" : "keyup", SDLK_RIGHT, SDL_SCANCODE_RIGHT); joy_right = right; }
}

// --- Touch → Pointer events ---
static void fire_touch_as_pointer(const char *type, float x, float y) {
    int wx = (int)(x * g_engine.screen_w);
    int wy = (int)(y * g_engine.screen_h);
    int gx, gy;
    window_to_game_coords(wx, wy, &gx, &gy);
    fire_mouse_event(type, gx, gy, 0);
}

void translate_sdl_event(SDL_Event *event) {
    switch (event->type) {
        case SDL_KEYDOWN:
            fire_key_event("keydown", &event->key);
            break;
        case SDL_KEYUP:
            fire_key_event("keyup", &event->key);
            break;
        case SDL_MOUSEMOTION: {
            static bool mouse_over = false;
            int gx, gy;
            window_to_game_coords(event->motion.x, event->motion.y, &gx, &gy);
            if (!mouse_over) {
                fire_mouse_event("pointerover", gx, gy, 0);
                fire_mouse_event("mouseover", gx, gy, 0);
                fire_mouse_event("pointerenter", gx, gy, 0);
                fire_mouse_event("mouseenter", gx, gy, 0);
                mouse_over = true;
            }
            fire_mouse_event("pointermove", gx, gy, 0);
            fire_mouse_event("mousemove", gx, gy, 0);
            break;
        }
        case SDL_MOUSEBUTTONDOWN: {
            int gx, gy;
            window_to_game_coords(event->button.x, event->button.y, &gx, &gy);
            fire_mouse_event("pointerdown", gx, gy, event->button.button - 1);
            fire_mouse_event("mousedown", gx, gy, event->button.button - 1);
            if (event->button.button == 3) {
                fire_mouse_event("contextmenu", gx, gy, 2);
            }
            break;
        }
        case SDL_MOUSEBUTTONUP: {
            int gx, gy;
            window_to_game_coords(event->button.x, event->button.y, &gx, &gy);
            fire_mouse_event("pointerup", gx, gy, event->button.button - 1);
            fire_mouse_event("mouseup", gx, gy, event->button.button - 1);
            break;
        }
        case SDL_FINGERDOWN:
            fire_touch_as_pointer("pointerdown", event->tfinger.x, event->tfinger.y);
            fire_touch_as_pointer("mousedown", event->tfinger.x, event->tfinger.y);
            break;
        case SDL_FINGERUP:
            fire_touch_as_pointer("pointerup", event->tfinger.x, event->tfinger.y);
            fire_touch_as_pointer("mouseup", event->tfinger.x, event->tfinger.y);
            break;
        case SDL_FINGERMOTION:
            fire_touch_as_pointer("pointermove", event->tfinger.x, event->tfinger.y);
            fire_touch_as_pointer("mousemove", event->tfinger.x, event->tfinger.y);
            break;
        case SDL_MOUSEWHEEL: {
            // Translate mouse wheel to 'wheel' event
            JSCContext *ctx = g_engine.js_ctx;
            int dx = event->wheel.x * 100;  // scale to match browser deltaX
            int dy = event->wheel.y * -100; // SDL Y is inverted vs DOM
            char js[1024];
            snprintf(js, sizeof(js),
                "(function() {"
                "  var e = { type:'wheel', deltaX:%d, deltaY:%d, deltaZ:0, deltaMode:0,"
                "    clientX:0, clientY:0, ctrlKey:false, shiftKey:false, altKey:false,"
                "    preventDefault:function(){}, stopPropagation:function(){} };"
                "  if (typeof _primaryCanvas !== 'undefined' && _primaryCanvas && _primaryCanvas._listeners && _primaryCanvas._listeners['wheel']) {"
                "    _primaryCanvas._listeners['wheel'].slice().forEach(function(cb){ cb(e); });"
                "  }"
                "  if (typeof __canvas !== 'undefined' && __canvas._listeners && __canvas._listeners['wheel']) {"
                "    __canvas._listeners['wheel'].slice().forEach(function(cb){ cb(e); });"
                "  }"
                "  if (window._eventListeners && window._eventListeners['wheel']) {"
                "    window._eventListeners['wheel'].slice().forEach(function(cb){ cb(e); });"
                "  }"
                "})();",
                dx, dy);
            JSCValue *r = jsc_context_evaluate(ctx, js, -1);
            if (r) g_object_unref(r);
            break;
        }
        case SDL_JOYBUTTONDOWN:
            translate_joy_button(event->jbutton.button, "keydown");
            break;
        case SDL_JOYBUTTONUP:
            translate_joy_button(event->jbutton.button, "keyup");
            break;
        case SDL_JOYAXISMOTION:
            translate_joy_axis(event->jaxis.axis, event->jaxis.value);
            break;
        case SDL_JOYHATMOTION:
            translate_joy_hat(event->jhat.value);
            break;
        case SDL_WINDOWEVENT:
            if (event->window.event == SDL_WINDOWEVENT_RESIZED) {
                g_engine.screen_w = event->window.data1;
                g_engine.screen_h = event->window.data2;
                // Fire resize event on window (use SDL window size, not game resolution)
                char resize_js[1024];
                snprintf(resize_js, sizeof(resize_js),
                    "(function(){"
                    "  window.innerWidth = window.outerWidth = screen.width = screen.availWidth = "
                    "    document.body.clientWidth = document.documentElement.clientWidth = %d;"
                    "  window.innerHeight = window.outerHeight = screen.height = screen.availHeight = "
                    "    document.body.clientHeight = document.documentElement.clientHeight = %d;"
                    "  var e = { type: 'resize' };"
                    "  if (window.onresize) window.onresize(e);"
                    "  if (window._eventListeners && window._eventListeners['resize']) {"
                    "    window._eventListeners['resize'].forEach(function(cb){ cb(e); });"
                    "  }"
                    "})();",
                    g_engine.screen_w, g_engine.screen_h);
                JSCValue *rr = jsc_context_evaluate(g_engine.js_ctx, resize_js, -1);
                if (rr) g_object_unref(rr);
            }
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_GAINED) {
                // Page Visibility: visible
                JSCValue *vr = jsc_context_evaluate(g_engine.js_ctx,
                    "(function(){"
                    "  document.hidden = false;"
                    "  document.visibilityState = 'visible';"
                    "  if (document._fireEvent) document._fireEvent('visibilitychange', { type: 'visibilitychange' });"
                    "  if (document.onvisibilitychange) document.onvisibilitychange({ type: 'visibilitychange' });"
                    "  if (window.onfocus) window.onfocus({ type: 'focus' });"
                    "  if (window._eventListeners && window._eventListeners['focus']) {"
                    "    window._eventListeners['focus'].forEach(function(cb){ cb({ type: 'focus' }); });"
                    "  }"
                    "})();", -1);
                if (vr) g_object_unref(vr);
            }
            if (event->window.event == SDL_WINDOWEVENT_FOCUS_LOST) {
                // Page Visibility: hidden
                JSCValue *vr = jsc_context_evaluate(g_engine.js_ctx,
                    "(function(){"
                    "  document.hidden = true;"
                    "  document.visibilityState = 'hidden';"
                    "  if (document._fireEvent) document._fireEvent('visibilitychange', { type: 'visibilitychange' });"
                    "  if (document.onvisibilitychange) document.onvisibilitychange({ type: 'visibilitychange' });"
                    "  if (window.onblur) window.onblur({ type: 'blur' });"
                    "  if (window._eventListeners && window._eventListeners['blur']) {"
                    "    window._eventListeners['blur'].forEach(function(cb){ cb({ type: 'blur' }); });"
                    "  }"
                    "})();", -1);
                if (vr) g_object_unref(vr);
            }
            break;
    }
}

void register_events_shim(JSCContext *ctx) {
    // Set up event listener registry on window
    jsc_context_evaluate(ctx,
        "window._eventListeners = {};"
        "window.addEventListener = function(type, cb) {"
        "  if (!window._eventListeners[type]) window._eventListeners[type] = [];"
        "  window._eventListeners[type].push(cb);"
        "};"
        "window.removeEventListener = function(type, cb) {"
        "  if (!window._eventListeners[type]) return;"
        "  var idx = window._eventListeners[type].indexOf(cb);"
        "  if (idx >= 0) window._eventListeners[type].splice(idx, 1);"
        "};"
        , -1);
}
