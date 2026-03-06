// PhaserQuest Runtime Polyfills
// Provides browser APIs that JSC doesn't have natively

// --- Safe callback wrappers ---
// Wrap setTimeout/setInterval/requestAnimationFrame so JS exceptions
// don't propagate into the C runtime (which would crash on unhandled exceptions).
// Browsers catch these at the event loop boundary; we replicate that behavior here.
// Store native C-level callback functions, then replace globals with safe wrappers
var __nativeSetTimeout = setTimeout;
var __nativeSetInterval = setInterval;
var __nativeClearTimeout = clearTimeout;
var __nativeClearInterval = clearInterval;
var __nativeRAF = requestAnimationFrame;
var __nativeCAF = cancelAnimationFrame;

window.setTimeout = self.setTimeout = setTimeout = function(fn, delay) {
    if (typeof fn !== 'function') return __nativeSetTimeout(fn, delay);
    var args = arguments.length > 2 ? Array.prototype.slice.call(arguments, 2) : [];
    return __nativeSetTimeout(function() {
        try { fn.apply(null, args); } catch(e) {
            console.error('[setTimeout]', e.message || e);
        }
    }, delay);
};

window.setInterval = self.setInterval = setInterval = function(fn, interval) {
    if (typeof fn !== 'function') return __nativeSetInterval(fn, interval);
    var args = arguments.length > 2 ? Array.prototype.slice.call(arguments, 2) : [];
    return __nativeSetInterval(function() {
        try { fn.apply(null, args); } catch(e) {
            console.error('[setInterval]', e.message || e);
            if (e.stack) console.error(e.stack);
        }
    }, interval);
};

window.clearTimeout = self.clearTimeout = clearTimeout = __nativeClearTimeout;
window.clearInterval = self.clearInterval = clearInterval = __nativeClearInterval;

window.requestAnimationFrame = self.requestAnimationFrame = requestAnimationFrame = function(fn) {
    return __nativeRAF(function(timestamp) {
        try { fn(timestamp); } catch(e) {
            console.error('[RAF]', e.message || e);
            if (e.stack) console.error(e.stack);
        }
    });
};

window.cancelAnimationFrame = self.cancelAnimationFrame = cancelAnimationFrame = __nativeCAF;

// --- Software Canvas2D Context ---
(function() {

function parseColor(str) {
    if (!str) return [0,0,0,255];
    if (str[0] === '#') {
        var hex = str.slice(1);
        if (hex.length === 3) hex = hex[0]+hex[0]+hex[1]+hex[1]+hex[2]+hex[2];
        return [parseInt(hex.substr(0,2),16), parseInt(hex.substr(2,2),16),
                parseInt(hex.substr(4,2),16), 255];
    }
    var m = str.match(/rgba?\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*(?:,\s*([\d.]+))?\s*\)/);
    if (m) return [+m[1], +m[2], +m[3], m[4] !== undefined ? Math.round(+m[4]*255) : 255];
    return [0,0,0,255];
}

function SoftCanvas2D(canvas) {
    this.canvas = canvas;
    this._fillColor = [0,0,0,255];
    this._strokeColor = [0,0,0,255];
    this._font = '10px sans-serif';
    this._textAlign = 'start';
    this._textBaseline = 'alphabetic';
    this._globalAlpha = 1.0;
    this._globalCompositeOperation = 'source-over';
    this._stack = [];
    this._transform = [1,0,0,1,0,0];
    this._imageSmoothingEnabled = true;
    this._lineWidth = 1;
    this._lineCap = 'butt';
    this._lineJoin = 'miter';
    this._shadowColor = 'rgba(0,0,0,0)';
    this._shadowBlur = 0;
    this._shadowOffsetX = 0;
    this._shadowOffsetY = 0;
    this._path = [];
    this._ensureBuffer();
}

SoftCanvas2D.prototype._ensureBuffer = function() {
    var w = this.canvas.width || 1;
    var h = this.canvas.height || 1;
    var size = w * h * 4;
    if (!this._buffer || this._buffer.length !== size) {
        this._arrayBuffer = new ArrayBuffer(size);
        this._buffer = new Uint8ClampedArray(this._arrayBuffer);
    }
};

Object.defineProperty(SoftCanvas2D.prototype, 'fillStyle', {
    get: function() {
        var c = this._fillColor;
        if (c[3] === 255) return '#' + ((1<<24)+(c[0]<<16)+(c[1]<<8)+c[2]).toString(16).slice(1);
        return 'rgba('+c[0]+','+c[1]+','+c[2]+','+(c[3]/255)+')';
    },
    set: function(v) {
        if (typeof v === 'string') this._fillColor = parseColor(v);
    }
});

Object.defineProperty(SoftCanvas2D.prototype, 'strokeStyle', {
    get: function() {
        var c = this._strokeColor;
        return 'rgba('+c[0]+','+c[1]+','+c[2]+','+(c[3]/255)+')';
    },
    set: function(v) {
        if (typeof v === 'string') this._strokeColor = parseColor(v);
    }
});

Object.defineProperty(SoftCanvas2D.prototype, 'font', {
    get: function() { return this._font; },
    set: function(v) { this._font = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'textAlign', {
    get: function() { return this._textAlign; },
    set: function(v) { this._textAlign = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'textBaseline', {
    get: function() { return this._textBaseline; },
    set: function(v) { this._textBaseline = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'globalAlpha', {
    get: function() { return this._globalAlpha; },
    set: function(v) { this._globalAlpha = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'globalCompositeOperation', {
    get: function() { return this._globalCompositeOperation; },
    set: function(v) { this._globalCompositeOperation = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'imageSmoothingEnabled', {
    get: function() { return this._imageSmoothingEnabled; },
    set: function(v) { this._imageSmoothingEnabled = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'lineWidth', {
    get: function() { return this._lineWidth; },
    set: function(v) { this._lineWidth = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'lineCap', {
    get: function() { return this._lineCap; },
    set: function(v) { this._lineCap = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'lineJoin', {
    get: function() { return this._lineJoin; },
    set: function(v) { this._lineJoin = v; }
});

Object.defineProperty(SoftCanvas2D.prototype, 'shadowColor', {
    get: function() { return this._shadowColor; },
    set: function(v) { this._shadowColor = v; }
});
Object.defineProperty(SoftCanvas2D.prototype, 'shadowBlur', {
    get: function() { return this._shadowBlur; },
    set: function(v) { this._shadowBlur = v; }
});
Object.defineProperty(SoftCanvas2D.prototype, 'shadowOffsetX', {
    get: function() { return this._shadowOffsetX; },
    set: function(v) { this._shadowOffsetX = v; }
});
Object.defineProperty(SoftCanvas2D.prototype, 'shadowOffsetY', {
    get: function() { return this._shadowOffsetY; },
    set: function(v) { this._shadowOffsetY = v; }
});

SoftCanvas2D.prototype.fillRect = function(x, y, w, h) {
    this._ensureBuffer();
    var cw = this.canvas.width || 1;
    var ch = this.canvas.height || 1;
    var c = this._fillColor;
    var a = Math.round(c[3] * this._globalAlpha);
    x = Math.max(0, Math.floor(x));
    y = Math.max(0, Math.floor(y));
    var x2 = Math.min(cw, Math.floor(x + w));
    var y2 = Math.min(ch, Math.floor(y + h));
    for (var py = y; py < y2; py++) {
        for (var px = x; px < x2; px++) {
            var i = (py * cw + px) * 4;
            this._buffer[i]   = c[0];
            this._buffer[i+1] = c[1];
            this._buffer[i+2] = c[2];
            this._buffer[i+3] = a;
        }
    }
};

SoftCanvas2D.prototype.clearRect = function(x, y, w, h) {
    this._ensureBuffer();
    var cw = this.canvas.width || 1;
    var ch = this.canvas.height || 1;
    x = Math.max(0, Math.floor(x));
    y = Math.max(0, Math.floor(y));
    var x2 = Math.min(cw, Math.floor(x + w));
    var y2 = Math.min(ch, Math.floor(y + h));
    for (var py = y; py < y2; py++) {
        for (var px = x; px < x2; px++) {
            var i = (py * cw + px) * 4;
            this._buffer[i] = this._buffer[i+1] = this._buffer[i+2] = this._buffer[i+3] = 0;
        }
    }
};

SoftCanvas2D.prototype.strokeRect = function(x, y, w, h) {
    // Stub - draw outline as filled rects
    var lw = this._lineWidth;
    var old = this._fillColor;
    this._fillColor = this._strokeColor;
    this.fillRect(x, y, w, lw);
    this.fillRect(x, y+h-lw, w, lw);
    this.fillRect(x, y, lw, h);
    this.fillRect(x+w-lw, y, lw, h);
    this._fillColor = old;
};

SoftCanvas2D.prototype.getImageData = function(x, y, w, h) {
    this._ensureBuffer();
    var cw = this.canvas.width || 1;
    var data = new Uint8ClampedArray(w * h * 4);
    for (var py = 0; py < h; py++) {
        for (var px = 0; px < w; px++) {
            var si = ((y+py) * cw + (x+px)) * 4;
            var di = (py * w + px) * 4;
            data[di]   = this._buffer[si]   || 0;
            data[di+1] = this._buffer[si+1] || 0;
            data[di+2] = this._buffer[si+2] || 0;
            data[di+3] = this._buffer[si+3] || 0;
        }
    }
    return { data: data, width: w, height: h };
};

SoftCanvas2D.prototype.putImageData = function(imageData, x, y) {
    this._ensureBuffer();
    var cw = this.canvas.width || 1;
    var w = imageData.width;
    var h = imageData.height;
    var src = imageData.data;
    for (var py = 0; py < h; py++) {
        for (var px = 0; px < w; px++) {
            var si = (py * w + px) * 4;
            var di = ((y+py) * cw + (x+px)) * 4;
            this._buffer[di]   = src[si];
            this._buffer[di+1] = src[si+1];
            this._buffer[di+2] = src[si+2];
            this._buffer[di+3] = src[si+3];
        }
    }
};

SoftCanvas2D.prototype.createImageData = function(w, h) {
    if (typeof w === 'object') { h = w.height; w = w.width; }
    return { data: new Uint8ClampedArray(w * h * 4), width: w, height: h };
};

SoftCanvas2D.prototype._parseFont = function() {
    var font = this._font || '10px sans-serif';
    var size = 10, family = 'sans-serif', bold = false, italic = false;
    var m = font.match(/(\d+)px/);
    if (m) size = +m[1];
    if (/\bbold\b/i.test(font)) bold = true;
    if (/\bitalic\b/i.test(font)) italic = true;
    // Extract family: everything after "Npx " (skip size and style keywords)
    var fm = font.match(/\d+px\s+(.+)/);
    if (fm) {
        family = fm[1].replace(/['"]/g, '').split(',')[0].trim();
    }
    return { size: size, family: family, bold: bold, italic: italic };
};

SoftCanvas2D.prototype.measureText = function(text) {
    var f = this._parseFont();
    if (typeof __textMeasure === 'function') {
        var result = __textMeasure(text || '', f.family, f.size, f.bold, f.italic);
        if (result) {
            return {
                width: result.width,
                actualBoundingBoxAscent: result.ascent,
                actualBoundingBoxDescent: result.descent
            };
        }
    }
    // Fallback: approximate
    var width = (text ? text.length : 0) * f.size * 0.6;
    return { width: width, actualBoundingBoxAscent: f.size * 0.8, actualBoundingBoxDescent: f.size * 0.2 };
};

// Built-in 5x7 bitmap font for basic text rendering
// Each character is a 5-wide, 7-tall bitmap stored as 7 bytes (bit 4=leftmost pixel)
var _font5x7 = {
' ':[0,0,0,0,0,0,0],'!':[4,4,4,4,0,0,4],'#':[10,31,10,10,31,10,0],
"'":[4,4,0,0,0,0,0],'"':[10,10,0,0,0,0,0],'(':[2,4,4,4,4,4,2],
')':[8,4,4,4,4,4,8],'*':[0,10,4,14,4,10,0],'+':[0,4,4,31,4,4,0],
',':[0,0,0,0,0,4,8],'-':[0,0,0,31,0,0,0],'.':[0,0,0,0,0,0,4],
'/':[1,1,2,4,8,16,16],
'0':[14,17,19,21,25,17,14],'1':[4,12,4,4,4,4,14],'2':[14,17,1,2,4,8,31],
'3':[14,17,1,6,1,17,14],'4':[2,6,10,18,31,2,2],'5':[31,16,30,1,1,17,14],
'6':[6,8,16,30,17,17,14],'7':[31,1,2,4,4,4,4],'8':[14,17,17,14,17,17,14],
'9':[14,17,17,15,1,2,12],':':[0,0,4,0,0,4,0],';':[0,0,4,0,0,4,8],
'<':[2,4,8,16,8,4,2],'=':[0,0,31,0,31,0,0],'>':[8,4,2,1,2,4,8],
'?':[14,17,1,2,4,0,4],'@':[14,17,23,21,23,16,14],
'A':[14,17,17,31,17,17,17],'B':[30,17,17,30,17,17,30],'C':[14,17,16,16,16,17,14],
'D':[30,17,17,17,17,17,30],'E':[31,16,16,30,16,16,31],'F':[31,16,16,30,16,16,16],
'G':[14,17,16,23,17,17,14],'H':[17,17,17,31,17,17,17],'I':[14,4,4,4,4,4,14],
'J':[7,2,2,2,2,18,12],'K':[17,18,20,24,20,18,17],'L':[16,16,16,16,16,16,31],
'M':[17,27,21,21,17,17,17],'N':[17,25,21,19,17,17,17],'O':[14,17,17,17,17,17,14],
'P':[30,17,17,30,16,16,16],'Q':[14,17,17,17,21,18,13],'R':[30,17,17,30,20,18,17],
'S':[14,17,16,14,1,17,14],'T':[31,4,4,4,4,4,4],'U':[17,17,17,17,17,17,14],
'V':[17,17,17,17,10,10,4],'W':[17,17,17,21,21,27,17],'X':[17,17,10,4,10,17,17],
'Y':[17,17,10,4,4,4,4],'Z':[31,1,2,4,8,16,31],
'[':[6,4,4,4,4,4,6],']':[12,4,4,4,4,4,12],'_':[0,0,0,0,0,0,31],
'a':[0,0,14,1,15,17,15],'b':[16,16,30,17,17,17,30],'c':[0,0,14,17,16,17,14],
'd':[1,1,15,17,17,17,15],'e':[0,0,14,17,31,16,14],'f':[6,9,8,28,8,8,8],
'g':[0,0,15,17,17,15,1,14],'h':[16,16,30,17,17,17,17],'i':[4,0,12,4,4,4,14],
'j':[2,0,6,2,2,2,18,12],'k':[16,16,18,20,24,20,18],'l':[12,4,4,4,4,4,14],
'm':[0,0,26,21,21,21,17],'n':[0,0,30,17,17,17,17],'o':[0,0,14,17,17,17,14],
'p':[0,0,30,17,17,30,16,16],'q':[0,0,15,17,17,15,1,1],'r':[0,0,22,25,16,16,16],
's':[0,0,15,16,14,1,30],'t':[8,8,28,8,8,9,6],'u':[0,0,17,17,17,17,15],
'v':[0,0,17,17,17,10,4],'w':[0,0,17,17,21,21,10],'x':[0,0,17,10,4,10,17],
'y':[0,0,17,17,17,15,1,14],'z':[0,0,31,2,4,8,31]
};

SoftCanvas2D.prototype.fillText = function(text, x, y, maxWidth) {
    if (!text) return;
    this._ensureBuffer();
    var cw = this.canvas.width || 1;
    var ch = this.canvas.height || 1;
    var c = this._fillColor;
    var alpha = Math.round(c[3] * this._globalAlpha);
    if (alpha === 0) return;

    var f = this._parseFont();

    // Try native TTF rendering
    if (typeof __textRender === 'function') {
        var rendered = __textRender(text, f.family, f.size, f.bold, f.italic, c[0], c[1], c[2], alpha);
        if (rendered && rendered.data) {
            var src = new Uint8Array(rendered.data);
            var rw = rendered.width;
            var rh = rendered.height;
            var ascent = rendered.ascent || rh;

            // Adjust y for baseline
            if (this._textBaseline === 'top') { /* y is top */ }
            else if (this._textBaseline === 'middle') { y -= rh / 2; }
            else { y -= ascent; } // alphabetic/bottom

            // Adjust x for alignment
            if (this._textAlign === 'center') { x -= rw / 2; }
            else if (this._textAlign === 'right' || this._textAlign === 'end') { x -= rw; }

            x = Math.round(x);
            y = Math.round(y);

            // Blit rendered text onto canvas buffer with alpha blending
            for (var py = 0; py < rh; py++) {
                var dy = y + py;
                if (dy < 0 || dy >= ch) continue;
                for (var px = 0; px < rw; px++) {
                    var dx = x + px;
                    if (dx < 0 || dx >= cw) continue;
                    var si = (py * rw + px) * 4;
                    var sa = src[si + 3];
                    if (sa === 0) continue;
                    var di = (dy * cw + dx) * 4;
                    if (sa === 255) {
                        this._buffer[di]     = src[si];
                        this._buffer[di + 1] = src[si + 1];
                        this._buffer[di + 2] = src[si + 2];
                        this._buffer[di + 3] = 255;
                    } else {
                        var inv = 255 - sa;
                        this._buffer[di]     = (src[si] * sa + this._buffer[di] * inv) >> 8;
                        this._buffer[di + 1] = (src[si+1] * sa + this._buffer[di+1] * inv) >> 8;
                        this._buffer[di + 2] = (src[si+2] * sa + this._buffer[di+2] * inv) >> 8;
                        this._buffer[di + 3] = Math.min(255, this._buffer[di+3] + sa);
                    }
                }
            }
            return;
        }
    }

    // Fallback: bitmap font
    var scale = Math.max(1, Math.round(f.size / 7));
    if (this._textBaseline === 'top') { /* y is already top */ }
    else if (this._textBaseline === 'middle') { y -= (7 * scale) / 2; }
    else { y -= 7 * scale; }

    var textWidth = text.length * 6 * scale;
    if (this._textAlign === 'center') { x -= textWidth / 2; }
    else if (this._textAlign === 'right' || this._textAlign === 'end') { x -= textWidth; }

    x = Math.round(x);
    y = Math.round(y);

    for (var ci = 0; ci < text.length; ci++) {
        var ch_code = text[ci];
        var glyph = _font5x7[ch_code] || _font5x7[ch_code.toUpperCase()] || _font5x7['?'];
        if (!glyph) continue;
        var ox = x + ci * 6 * scale;
        for (var row = 0; row < 7; row++) {
            var bits = glyph[row] || 0;
            for (var col = 0; col < 5; col++) {
                if (bits & (16 >> col)) {
                    for (var sy = 0; sy < scale; sy++) {
                        for (var sx = 0; sx < scale; sx++) {
                            var bpx = ox + col * scale + sx;
                            var bpy = y + row * scale + sy;
                            if (bpx >= 0 && bpx < cw && bpy >= 0 && bpy < ch) {
                                var idx = (bpy * cw + bpx) * 4;
                                this._buffer[idx]   = c[0];
                                this._buffer[idx+1] = c[1];
                                this._buffer[idx+2] = c[2];
                                this._buffer[idx+3] = alpha;
                            }
                        }
                    }
                }
            }
        }
    }
};
SoftCanvas2D.prototype.strokeText = function(text, x, y, maxWidth) {
    // Use fillText with stroke color
    var old = this._fillColor;
    this._fillColor = this._strokeColor;
    this.fillText(text, x, y, maxWidth);
    this._fillColor = old;
};

SoftCanvas2D.prototype.drawImage = function() {};

SoftCanvas2D.prototype.save = function() {
    this._stack.push({
        fill: this._fillColor.slice(), stroke: this._strokeColor.slice(),
        alpha: this._globalAlpha, transform: this._transform.slice(),
        font: this._font, textAlign: this._textAlign, textBaseline: this._textBaseline
    });
};
SoftCanvas2D.prototype.restore = function() {
    var s = this._stack.pop();
    if (s) {
        this._fillColor = s.fill; this._strokeColor = s.stroke;
        this._globalAlpha = s.alpha; this._transform = s.transform;
        this._font = s.font; this._textAlign = s.textAlign; this._textBaseline = s.textBaseline;
    }
};

SoftCanvas2D.prototype.translate = function(x, y) { this._transform[4] += x; this._transform[5] += y; };
SoftCanvas2D.prototype.scale = function(x, y) { this._transform[0] *= x; this._transform[3] *= y; };
SoftCanvas2D.prototype.rotate = function(a) {};
SoftCanvas2D.prototype.setTransform = function(a,b,c,d,e,f) { this._transform = [a,b,c,d,e,f]; };
SoftCanvas2D.prototype.resetTransform = function() { this._transform = [1,0,0,1,0,0]; };
SoftCanvas2D.prototype.transform = function(a,b,c,d,e,f) {};

SoftCanvas2D.prototype.beginPath = function() { this._path = []; };
SoftCanvas2D.prototype.closePath = function() {};
SoftCanvas2D.prototype.moveTo = function(x, y) { this._path.push({t:'m',x:x,y:y}); };
SoftCanvas2D.prototype.lineTo = function(x, y) { this._path.push({t:'l',x:x,y:y}); };
SoftCanvas2D.prototype.arc = function() {};
SoftCanvas2D.prototype.arcTo = function() {};
SoftCanvas2D.prototype.quadraticCurveTo = function() {};
SoftCanvas2D.prototype.bezierCurveTo = function() {};
SoftCanvas2D.prototype.rect = function(x,y,w,h) {};
SoftCanvas2D.prototype.fill = function() {};
SoftCanvas2D.prototype.stroke = function() {};
SoftCanvas2D.prototype.clip = function() {};
SoftCanvas2D.prototype.isPointInPath = function() { return false; };

SoftCanvas2D.prototype.createLinearGradient = function(x0,y0,x1,y1) {
    return { addColorStop: function(){} };
};
SoftCanvas2D.prototype.createRadialGradient = function(x0,y0,r0,x1,y1,r1) {
    return { addColorStop: function(){} };
};
SoftCanvas2D.prototype.createPattern = function() { return {}; };

SoftCanvas2D.prototype.getLineDash = function() { return []; };
SoftCanvas2D.prototype.setLineDash = function() {};

// Expose for use by canvas shim
window.__SoftCanvas2D = SoftCanvas2D;

// --- Canvas element factory ---
var _primaryCanvas = null;

window.__createCanvas = function() {
    var canvas = {
        tagName: 'CANVAS',
        nodeName: 'CANVAS',
        width: 1,
        height: 1,
        style: {},
        _listeners: {},
        _ctx2d: null,
        _isPrimary: false
    };

    canvas.getContext = function(type, attrs) {
        if (type === '2d') {
            if (!this._ctx2d) this._ctx2d = new SoftCanvas2D(this);
            return this._ctx2d;
        }
        if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl') {
            // Return the native WebGL context for any canvas requesting it
            // The actual GL context is global (one per SDL window)
            if (typeof __canvas !== 'undefined') {
                var glCtx = __canvas.getContext('webgl');
                if (glCtx) {
                    // Track which canvas is the "active" WebGL canvas
                    _primaryCanvas = this;
                    this._isPrimary = true;
                    this.width = this.width || innerWidth;
                    this.height = this.height || innerHeight;
                    glCtx.canvas = this;
                    glCtx.drawingBufferWidth = this.width;
                    glCtx.drawingBufferHeight = this.height;
                    return glCtx;
                }
            }
            return null;
        }
        return null;
    };

    canvas.getBoundingClientRect = function() {
        return { left:0, top:0, right:this.width, bottom:this.height,
                 width:this.width, height:this.height, x:0, y:0 };
    };

    canvas.addEventListener = function(type, cb, opts) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(cb);
        // Also register on window for keyboard events
        if (type === 'keydown' || type === 'keyup') {
            window.addEventListener(type, cb);
        }
    };
    canvas.removeEventListener = function(type, cb) {
        if (!this._listeners[type]) return;
        var idx = this._listeners[type].indexOf(cb);
        if (idx >= 0) this._listeners[type].splice(idx, 1);
    };

    canvas.setAttribute = function() {};
    canvas.removeAttribute = function() {};
    canvas.focus = function() {};
    canvas.blur = function() {};
    canvas.classList = { add: function(){}, remove: function(){}, contains: function(){ return false; } };
    canvas.parentElement = (typeof document !== 'undefined' && document.body) ? document.body : null;
    canvas.parentNode = canvas.parentElement;
    canvas.ownerDocument = typeof document !== 'undefined' ? document : null;
    canvas.toDataURL = function() { return 'data:image/png;base64,'; };

    // Expose 2D canvas pixel data for texImage2D upload
    Object.defineProperty(canvas, '_pixelData', {
        get: function() {
            if (this._ctx2d && this._ctx2d._arrayBuffer) {
                return this._ctx2d._arrayBuffer;
            }
            return null;
        }
    });

    return canvas;
};

window.__createStubElement = function(tag) {
    var el = {
        tagName: tag.toUpperCase(),
        nodeName: tag.toUpperCase(),
        style: {},
        children: [],
        childNodes: [],
        _listeners: {},
        innerHTML: '',
        innerText: '',
        textContent: '',
        className: ''
    };
    el.appendChild = function(child) { this.children.push(child); return child; };
    el.removeChild = function(child) {
        var i = this.children.indexOf(child);
        if (i >= 0) this.children.splice(i, 1);
        return child;
    };
    el.insertBefore = function(child) { this.children.push(child); return child; };
    el.contains = function() { return false; };
    el.getAttribute = function() { return null; };
    el.setAttribute = function() {};
    el.removeAttribute = function() {};
    el.getBoundingClientRect = function() { return { left:0, top:0, right:0, bottom:0, width:0, height:0, x:0, y:0 }; };
    el.addEventListener = function(type, cb) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(cb);
    };
    el.removeEventListener = function(type, cb) {
        if (!this._listeners[type]) return;
        var idx = this._listeners[type].indexOf(cb);
        if (idx >= 0) this._listeners[type].splice(idx, 1);
    };
    el.classList = { add: function(){}, remove: function(){}, contains: function(){ return false; } };
    el.cloneNode = function() { return __createStubElement(tag); };
    el.focus = function() {};
    el.blur = function() {};
    el.dispatchEvent = function() {};
    el.parentElement = null;
    el.parentNode = null;

    // Audio element needs canPlayType for Phaser's device detection
    if (tag.toLowerCase() === 'audio') {
        el.canPlayType = function(mime) {
            if (mime.indexOf('audio/ogg') >= 0) return 'maybe';
            if (mime.indexOf('audio/mpeg') >= 0) return 'maybe';
            if (mime.indexOf('audio/wav') >= 0) return 'maybe';
            if (mime.indexOf('audio/mp4') >= 0) return 'maybe';
            if (mime.indexOf('audio/aac') >= 0) return 'maybe';
            if (mime.indexOf('audio/webm') >= 0) return 'maybe';
            if (mime.indexOf('audio/x-m4a') >= 0) return 'maybe';
            return '';
        };
        el.play = function() { return Promise.resolve(); };
        el.pause = function() {};
        el.load = function() {};
        el.volume = 1;
        el.muted = false;
        el.paused = true;
        el.src = '';
    }
    return el;
};

// Audio constructor (Phaser checks window['Audio'])
window.Audio = function(src) {
    var el = __createStubElement('audio');
    if (src) el.src = src;
    return el;
};

})();

// --- Image src setter ---
(function() {
    var _OrigImage = Image;
    window.Image = function(w, h) {
        var img = _OrigImage(w, h);
        var _src = '';
        var _attrs = {};
        Object.defineProperty(img, 'src', {
            get: function() { return _src; },
            set: function(val) {
                _src = val;
                if (val) {
                    var self = img;
                    if (val.indexOf('blob:') === 0 && __blobStore[val]) {
                        // Blob URL - decode from stored blob data
                        setTimeout(function() {
                            var blob = __blobStore[val];
                            if (blob instanceof ArrayBuffer) {
                                __imageLoadBuffer(self, new Uint8Array(blob));
                            } else if (blob && blob._parts) {
                                // Blob object - get underlying data
                                blob.arrayBuffer().then(function(buf) {
                                    __imageLoadBuffer(self, new Uint8Array(buf));
                                });
                            } else if (blob && blob.byteLength !== undefined) {
                                __imageLoadBuffer(self, new Uint8Array(blob));
                            } else {
                                self.complete = true;
                                if (self.onerror) self.onerror({ type: 'error', target: self });
                            }
                        }, 0);
                    } else if (val.indexOf('data:') === 0) {
                        // Handle data URI - decode base64 PNG to get dimensions
                        setTimeout(function() {
                            self.complete = true;
                            // Extract dimensions from base64 PNG header if possible
                            var b64 = val.indexOf(',');
                            if (b64 >= 0) {
                                try {
                                    var raw = atob(val.substring(b64 + 1));
                                    var bytes = new Uint8Array(raw.length);
                                    for (var i = 0; i < raw.length; i++) bytes[i] = raw.charCodeAt(i);
                                    // PNG: width at offset 16-19, height at 20-23 (big-endian)
                                    if (raw.length > 24 && bytes[0] === 0x89 && bytes[1] === 0x50) {
                                        var w = (bytes[16] << 24 | bytes[17] << 16 | bytes[18] << 8 | bytes[19]) >>> 0;
                                        var h = (bytes[20] << 24 | bytes[21] << 16 | bytes[22] << 8 | bytes[23]) >>> 0;
                                        self.width = w; self.height = h;
                                        self.naturalWidth = w; self.naturalHeight = h;
                                        // Create a simple RGBA pixel buffer
                                        var pixelData = new Uint8Array(w * h * 4);
                                        self._pixelData = pixelData.buffer;
                                    }
                                } catch(e) {}
                            }
                            if (self.onload) self.onload({ type: 'load', target: self });
                        }, 0);
                    } else {
                        setTimeout(function() { __imageLoad(self, val); }, 0);
                    }
                }
            }
        });
        img.addEventListener = function(type, cb) {
            if (type === 'load') img.onload = cb;
            else if (type === 'error') img.onerror = cb;
        };
        img.removeEventListener = function() {};
        img.setAttribute = function(name, value) { _attrs[name] = value; img[name] = value; };
        img.getAttribute = function(name) { return _attrs[name] !== undefined ? _attrs[name] : null; };
        img.removeAttribute = function(name) { delete _attrs[name]; };
        img.hasAttribute = function(name) { return name in _attrs; };
        img.nodeName = 'IMG';
        img.tagName = 'IMG';
        return img;
    };
    window.HTMLImageElement = window.Image;
})();

// --- XMLHttpRequest ---
window.XMLHttpRequest = function() {
    this.readyState = 0;
    this.status = 0;
    this.statusText = '';
    this.responseText = '';
    this.response = null;
    this.responseType = '';
    this.responseURL = '';
    this.onload = null;
    this.onerror = null;
    this.onreadystatechange = null;
    this.onprogress = null;
    this.ontimeout = null;
    this.onabort = null;
    this.timeout = 0;
    this.withCredentials = false;
    this._method = 'GET';
    this._url = '';
    this._headers = {};
    this._listeners = {};
};

XMLHttpRequest.prototype.open = function(method, url, async) {
    this._method = method;
    this._url = url;
    this.readyState = 1;
    this.responseURL = url;
};

XMLHttpRequest.prototype.setRequestHeader = function(name, value) {
    this._headers[name] = value;
};

XMLHttpRequest.prototype.getResponseHeader = function(name) {
    if (name.toLowerCase() === 'content-type') {
        var url = this._url.toLowerCase();
        if (url.endsWith('.json')) return 'application/json';
        if (url.endsWith('.js')) return 'application/javascript';
        if (url.endsWith('.png')) return 'image/png';
        if (url.endsWith('.jpg') || url.endsWith('.jpeg')) return 'image/jpeg';
        if (url.endsWith('.xml')) return 'application/xml';
        if (url.endsWith('.txt') || url.endsWith('.fnt')) return 'text/plain';
        if (url.endsWith('.csv')) return 'text/csv';
        if (url.endsWith('.ogg')) return 'audio/ogg';
        if (url.endsWith('.mp3')) return 'audio/mpeg';
        if (url.endsWith('.wav')) return 'audio/wav';
        return 'application/octet-stream';
    }
    return null;
};

XMLHttpRequest.prototype.getAllResponseHeaders = function() {
    return 'content-type: ' + (this.getResponseHeader('content-type') || 'text/plain') + '\r\n';
};

XMLHttpRequest.prototype.overrideMimeType = function(type) {};

XMLHttpRequest.prototype.send = function(body) {
    var xhr = this;
    setTimeout(function() {
        var data;
        if (xhr.responseType === 'arraybuffer' || xhr.responseType === 'blob') {
            data = __readFileBuffer(xhr._url);
        } else {
            data = __readFileText(xhr._url);
        }

        if (data !== null) {
            xhr.status = 200;
            xhr.statusText = 'OK';
            if (xhr.responseType === 'arraybuffer') {
                xhr.response = data;
            } else if (xhr.responseType === 'blob') {
                // Wrap ArrayBuffer in a Blob for blob responseType
                var ct = xhr.getResponseHeader('content-type') || 'application/octet-stream';
                xhr.response = new Blob([data], { type: ct });
            } else if (xhr.responseType === 'json') {
                xhr.responseText = data;
                try { xhr.response = JSON.parse(data); } catch(e) { xhr.response = null; }
            } else {
                xhr.responseText = data;
                xhr.response = data;
            }
            // Auto-parse XML for responseXML
            if (xhr.responseText && (xhr._url.indexOf('.xml') >= 0 || xhr._url.indexOf('.fnt') >= 0)) {
                try {
                    xhr.responseXML = new DOMParser().parseFromString(xhr.responseText, 'text/xml');
                } catch(e) { xhr.responseXML = null; }
            }
        } else {
            xhr.status = 404;
            xhr.statusText = 'Not Found';
            xhr.responseText = '';
            xhr.response = xhr.responseType === 'arraybuffer' ? new ArrayBuffer(0) : '';
        }

        xhr.readyState = 4;
        var evt = { type: 'readystatechange', target: xhr, currentTarget: xhr };
        if (xhr.onreadystatechange) xhr.onreadystatechange(evt);
        xhr._fireEvent('readystatechange', evt);

        if (xhr.status >= 200 && xhr.status < 400) {
            var loadEvt = { type: 'load', target: xhr, currentTarget: xhr, lengthComputable: false, loaded: 0, total: 0 };
            if (xhr.onload) xhr.onload(loadEvt);
            xhr._fireEvent('load', loadEvt);
        } else {
            var errEvt = { type: 'error', target: xhr, currentTarget: xhr };
            if (xhr.onerror) xhr.onerror(errEvt);
            xhr._fireEvent('error', errEvt);
        }
    }, 0);
};

XMLHttpRequest.prototype.abort = function() {
    this.readyState = 0;
};

XMLHttpRequest.prototype._fireEvent = function(type, evt) {
    var cbs = this._listeners[type];
    if (cbs) {
        for (var i = 0; i < cbs.length; i++) cbs[i](evt);
    }
};

XMLHttpRequest.prototype.addEventListener = function(type, cb) {
    if (!this._listeners[type]) this._listeners[type] = [];
    this._listeners[type].push(cb);
};

XMLHttpRequest.prototype.removeEventListener = function(type, cb) {
    if (!this._listeners[type]) return;
    var idx = this._listeners[type].indexOf(cb);
    if (idx >= 0) this._listeners[type].splice(idx, 1);
};

XMLHttpRequest.UNSENT = 0;
XMLHttpRequest.OPENED = 1;
XMLHttpRequest.HEADERS_RECEIVED = 2;
XMLHttpRequest.LOADING = 3;
XMLHttpRequest.DONE = 4;

// --- fetch ---
window.fetch = function(url, opts) {
    return new Promise(function(resolve, reject) {
        var xhr = new XMLHttpRequest();
        xhr.open((opts && opts.method) || 'GET', url);
        if (opts && opts.headers) {
            for (var k in opts.headers) xhr.setRequestHeader(k, opts.headers[k]);
        }
        xhr.onload = function() {
            resolve({
                ok: xhr.status >= 200 && xhr.status < 300,
                status: xhr.status,
                statusText: xhr.statusText,
                url: url,
                text: function() { return Promise.resolve(xhr.responseText); },
                json: function() { return Promise.resolve(JSON.parse(xhr.responseText)); },
                arrayBuffer: function() { return Promise.resolve(xhr.response); },
                blob: function() { return Promise.resolve({ size: xhr.responseText.length, type: '' }); },
                headers: { get: function(n) { return xhr.getResponseHeader(n); } }
            });
        };
        xhr.onerror = function() { reject(new Error('Network error')); };
        xhr.send(opts && opts.body);
    });
};

// --- TextDecoder / TextEncoder ---
if (typeof TextDecoder === 'undefined') {
    window.TextDecoder = function(encoding) {
        this.encoding = encoding || 'utf-8';
    };
    TextDecoder.prototype.decode = function(buffer) {
        if (!buffer) return '';
        var bytes;
        if (buffer instanceof ArrayBuffer) {
            bytes = new Uint8Array(buffer);
        } else if (buffer.buffer) {
            bytes = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
        } else {
            return '';
        }
        var result = '';
        for (var i = 0; i < bytes.length; i++) {
            result += String.fromCharCode(bytes[i]);
        }
        return result;
    };
}

if (typeof TextEncoder === 'undefined') {
    window.TextEncoder = function() {
        this.encoding = 'utf-8';
    };
    TextEncoder.prototype.encode = function(str) {
        var arr = new Uint8Array(str.length);
        for (var i = 0; i < str.length; i++) {
            arr[i] = str.charCodeAt(i) & 0xFF;
        }
        return arr;
    };
}

// --- URL ---
var __blobStore = {};
if (typeof URL === 'undefined') {
    window.URL = function(url, base) {
        this.href = url;
        this.pathname = url;
        this.toString = function() { return url; };
    };
    URL.createObjectURL = function(blob) {
        var id = 'blob:null/' + (++URL._nextId);
        __blobStore[id] = blob;
        return id;
    };
    URL.revokeObjectURL = function(url) {
        delete __blobStore[url];
    };
    URL._nextId = 0;
}

// --- Blob ---
if (typeof Blob === 'undefined') {
    window.Blob = function(parts, options) {
        this.type = (options && options.type) || '';
        this._parts = parts || [];
        this.size = 0;
        for (var i = 0; i < this._parts.length; i++) {
            var p = this._parts[i];
            this.size += (p.byteLength !== undefined) ? p.byteLength : (p.length || 0);
        }
    };
    Blob.prototype.arrayBuffer = function() {
        var parts = this._parts;
        var totalLen = this.size;
        var result = new Uint8Array(totalLen);
        var offset = 0;
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i];
            if (p instanceof ArrayBuffer) {
                result.set(new Uint8Array(p), offset);
                offset += p.byteLength;
            } else if (ArrayBuffer.isView(p)) {
                result.set(new Uint8Array(p.buffer, p.byteOffset, p.byteLength), offset);
                offset += p.byteLength;
            } else if (typeof p === 'string') {
                for (var j = 0; j < p.length; j++) result[offset++] = p.charCodeAt(j) & 0xff;
            }
        }
        return Promise.resolve(result.buffer);
    };
    Blob.prototype.text = function() {
        return this.arrayBuffer().then(function(buf) {
            return new TextDecoder().decode(buf);
        });
    };
    Blob.prototype.slice = function(start, end, type) {
        return new Blob([], { type: type || this.type });
    };
}

// --- FileReader ---
if (typeof FileReader === 'undefined') {
    window.FileReader = function() {
        this.readyState = 0; // EMPTY
        this.result = null;
        this.error = null;
        this.onload = null;
        this.onerror = null;
        this.onloadend = null;
        this.onprogress = null;
        this._listeners = {};
    };
    FileReader.EMPTY = 0;
    FileReader.LOADING = 1;
    FileReader.DONE = 2;
    FileReader.prototype.addEventListener = function(type, fn) {
        if (!this._listeners[type]) this._listeners[type] = [];
        this._listeners[type].push(fn);
    };
    FileReader.prototype.removeEventListener = function(type, fn) {
        if (!this._listeners[type]) return;
        this._listeners[type] = this._listeners[type].filter(function(f) { return f !== fn; });
    };
    FileReader.prototype._fire = function(type) {
        var ev = { type: type, target: this };
        if (this['on' + type]) this['on' + type](ev);
        var list = this._listeners[type];
        if (list) for (var i = 0; i < list.length; i++) list[i](ev);
    };
    FileReader.prototype._finish = function(result) {
        this.result = result;
        this.readyState = FileReader.DONE;
        this._fire('load');
        this._fire('loadend');
    };
    FileReader.prototype.readAsArrayBuffer = function(blob) {
        var self = this;
        self.readyState = FileReader.LOADING;
        if (blob && typeof blob.arrayBuffer === 'function') {
            blob.arrayBuffer().then(function(buf) { self._finish(buf); });
        } else {
            self._finish(new ArrayBuffer(0));
        }
    };
    FileReader.prototype.readAsText = function(blob) {
        var self = this;
        self.readyState = FileReader.LOADING;
        if (blob && typeof blob.text === 'function') {
            blob.text().then(function(t) { self._finish(t); });
        } else {
            self._finish('');
        }
    };
    FileReader.prototype.readAsDataURL = function(blob) {
        var self = this;
        self.readyState = FileReader.LOADING;
        // Return a minimal data URL
        self._finish('data:' + (blob && blob.type || '') + ';base64,');
    };
}

// --- atob / btoa ---
if (typeof atob === 'undefined') {
    var chars = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=';
    window.atob = function(input) {
        var str = String(input).replace(/[=]+$/, '');
        var output = '';
        for (var bc = 0, bs, buffer, idx = 0; buffer = str.charAt(idx++);
             ~buffer && (bs = bc % 4 ? bs * 64 + buffer : buffer, bc++ % 4)
                ? output += String.fromCharCode(255 & bs >> (-2 * bc & 6)) : 0) {
            buffer = chars.indexOf(buffer);
        }
        return output;
    };
    window.btoa = function(input) {
        var str = String(input);
        var output = '';
        for (var block, charCode, idx = 0, map = chars;
             str.charAt(idx | 0) || (map = '=', idx % 1);
             output += map.charAt(63 & block >> 8 - idx % 1 * 8)) {
            charCode = str.charCodeAt(idx += 3/4);
            block = block << 8 | charCode;
        }
        return output;
    };
}

// --- HTMLElement stubs ---
window.HTMLElement = function() {};
window.HTMLCanvasElement = function() {};

// Feature detection: Phaser checks these exist
window.CanvasRenderingContext2D = window.__SoftCanvas2D;
window.WebGLRenderingContext = function() {};
window.WebGL2RenderingContext = function() {};
window.WebGLTexture = function() {};
window.WebGLBuffer = function() {};
window.WebGLFramebuffer = function() {};
window.WebGLRenderbuffer = function() {};
window.WebGLProgram = function() {};
window.WebGLShader = function() {};
window.WebGLUniformLocation = function() {};
window.HTMLDivElement = function() {};
window.HTMLVideoElement = function() {};
window.MouseEvent = function(type, init) { this.type = type; };
window.KeyboardEvent = function(type, init) { this.type = type; };
window.PointerEvent = window.MouseEvent;
window.TouchEvent = function(type, init) { this.type = type; };
window.WheelEvent = function(type, init) { this.type = type; };
window.Event = function(type, init) { this.type = type; };
window.CustomEvent = function(type, init) { this.type = type; this.detail = init && init.detail; };
window.FocusEvent = window.Event;
window.DOMParser = function() {};
DOMParser.prototype.parseFromString = function(str, type) {
    // Minimal XML/HTML parser for Phaser bitmap fonts, sprite atlases etc.
    var allElements = [];

    function XMLNode(tagName) {
        this.tagName = tagName;
        this.nodeName = tagName;
        this.attributes = {};
        this.childNodes = [];
        this.children = [];
        this.textContent = '';
    }
    XMLNode.prototype.getAttribute = function(name) {
        return this.attributes[name] !== undefined ? this.attributes[name] : null;
    };
    XMLNode.prototype.setAttribute = function(name, val) { this.attributes[name] = val; };
    XMLNode.prototype.getElementsByTagName = function(name) {
        var results = [];
        function walk(node) {
            if (node.tagName && node.tagName.toLowerCase() === name.toLowerCase()) results.push(node);
            for (var i = 0; i < node.childNodes.length; i++) walk(node.childNodes[i]);
        }
        for (var i = 0; i < this.childNodes.length; i++) walk(this.childNodes[i]);
        return results;
    };
    XMLNode.prototype.querySelector = function(sel) {
        var tag = sel.replace(/[^a-zA-Z]/g, '');
        var res = this.getElementsByTagName(tag);
        return res.length > 0 ? res[0] : null;
    };
    XMLNode.prototype.querySelectorAll = function(sel) {
        var tag = sel.replace(/[^a-zA-Z]/g, '');
        return this.getElementsByTagName(tag);
    };

    var doc = new XMLNode('#document');
    var stack = [doc];

    // Match tags: opening, self-closing, closing
    var re = /<(\/?)([a-zA-Z_][\w:-]*)((?:\s+[a-zA-Z_][\w:-]*\s*=\s*"[^"]*")*)\s*(\/?)>|([^<]+)/g;
    var m;
    while ((m = re.exec(str)) !== null) {
        if (m[5] !== undefined) {
            // Text node
            var parent = stack[stack.length - 1];
            if (parent) parent.textContent += m[5];
            continue;
        }
        var isClose = m[1] === '/';
        var tagName = m[2];
        var attrsStr = m[3] || '';
        var selfClose = m[4] === '/';

        if (isClose) {
            if (stack.length > 1) stack.pop();
            continue;
        }

        var node = new XMLNode(tagName);
        // Parse attributes
        var attrRe = /([a-zA-Z_][\w:-]*)\s*=\s*"([^"]*)"/g;
        var am;
        while ((am = attrRe.exec(attrsStr)) !== null) {
            node.attributes[am[1]] = am[2];
        }

        var parent = stack[stack.length - 1];
        parent.childNodes.push(node);
        parent.children.push(node);

        if (!selfClose) {
            stack.push(node);
        }
    }

    doc.documentElement = doc.childNodes[0] || doc;
    return doc;
};

// --- requestIdleCallback ---
window.requestIdleCallback = window.requestIdleCallback || function(cb) { return setTimeout(cb, 1); };
window.cancelIdleCallback = window.cancelIdleCallback || clearTimeout;

// --- Promise.allSettled polyfill ---
if (!Promise.allSettled) {
    Promise.allSettled = function(promises) {
        return Promise.all(promises.map(function(p) {
            return Promise.resolve(p).then(
                function(value) { return { status: 'fulfilled', value: value }; },
                function(reason) { return { status: 'rejected', reason: reason }; }
            );
        }));
    };
}

// --- MutationObserver stub ---
window.MutationObserver = window.MutationObserver || function() {
    this.observe = function() {};
    this.disconnect = function() {};
};

// --- ResizeObserver stub ---
window.ResizeObserver = window.ResizeObserver || function(cb) {
    this.observe = function() {};
    this.unobserve = function() {};
    this.disconnect = function() {};
};

// --- IntersectionObserver stub ---
window.IntersectionObserver = window.IntersectionObserver || function(cb) {
    this.observe = function() {};
    this.unobserve = function() {};
    this.disconnect = function() {};
};

// --- localStorage stub ---
if (typeof localStorage === 'undefined') {
    var _storage = {};
    window.localStorage = {
        getItem: function(k) { return _storage.hasOwnProperty(k) ? _storage[k] : null; },
        setItem: function(k, v) { _storage[k] = String(v); },
        removeItem: function(k) { delete _storage[k]; },
        clear: function() { _storage = {}; },
        get length() { return Object.keys(_storage).length; },
        key: function(i) { return Object.keys(_storage)[i] || null; }
    };
}

// --- navigator stubs ---
if (typeof navigator !== 'undefined') {
    navigator.getGamepads = navigator.getGamepads || function() { return []; };
    navigator.vibrate = navigator.vibrate || function() { return true; };
    navigator.clipboard = navigator.clipboard || {
        writeText: function() { return Promise.resolve(); },
        readText: function() { return Promise.resolve(''); }
    };
}

// --- document.createElement video canPlayType ---
// Phaser checks video.canPlayType for device detection
if (typeof document !== 'undefined') {
    var _origCreate = document.createElement;
    document.createElement = function(tag) {
        var el = _origCreate(tag);
        if (tag.toLowerCase() === 'video' && !el.canPlayType) {
            el.canPlayType = function(mime) { return ''; };
            el.play = function() { return Promise.resolve(); };
            el.pause = function() {};
            el.load = function() {};
        }
        return el;
    };
}

// --- Missing browser globals ---
window.alert = window.alert || function(msg) { console.log('[alert] ' + msg); };
window.confirm = window.confirm || function(msg) { console.log('[confirm] ' + msg); return true; };
window.prompt = window.prompt || function(msg) { console.log('[prompt] ' + msg); return null; };
window.focus = window.focus || function() {};
window.blur = window.blur || function() {};

console.log('[PhaserQuest] Polyfills loaded');
