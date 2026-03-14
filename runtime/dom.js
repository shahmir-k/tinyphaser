// dom.js - JS DOM API backed by litehtml via native bridge
// Provides Node, Element, Text, Document etc. that Angular/React/Vue need
(function() {
'use strict';

// Node cache: maps litehtml element ID → JS DOMNode wrapper
var nodeCache = {};

// Node type constants
var ELEMENT_NODE = 1;
var TEXT_NODE = 3;
var COMMENT_NODE = 8;
var DOCUMENT_NODE = 9;
var DOCUMENT_FRAGMENT_NODE = 11;

// ---------------------------------------------------------------------------
// CSSStyleDeclaration
// ---------------------------------------------------------------------------
function CSSStyleDeclaration(elNode) {
    this._el = elNode;
    this._props = {};
}
CSSStyleDeclaration.prototype.getPropertyValue = function(prop) {
    return this._props[prop] || '';
};
CSSStyleDeclaration.prototype.setProperty = function(prop, value) {
    this._props[prop] = value;
    this._syncToAttr();
};
CSSStyleDeclaration.prototype.removeProperty = function(prop) {
    var old = this._props[prop] || '';
    delete this._props[prop];
    this._syncToAttr();
    return old;
};
CSSStyleDeclaration.prototype._syncToAttr = function() {
    var parts = [];
    for (var k in this._props) {
        if (this._props[k]) parts.push(k + ': ' + this._props[k]);
    }
    if (this._el && this._el._id) {
        __domSetAttr(this._el._id, 'style', parts.join('; '));
    }
};
// Allow style.display = 'none' etc via proxy-like setter
// We use defineProperty for common CSS properties
var cssProps = ['display','visibility','position','top','left','right','bottom',
    'width','height','margin','padding','border','background','color','font',
    'fontSize','fontFamily','fontWeight','fontStyle','textAlign','overflow',
    'zIndex','opacity','transform','transition','cursor','pointerEvents',
    'boxSizing','flexDirection','justifyContent','alignItems','flex','gap',
    'gridTemplateColumns','gridTemplateRows','maxWidth','maxHeight',
    'minWidth','minHeight','lineHeight','textDecoration','whiteSpace',
    'borderRadius','boxShadow','outline','userSelect','float','clear',
    'verticalAlign','letterSpacing','textTransform','wordBreak','overflowX',
    'overflowY','backgroundImage','backgroundColor','backgroundSize',
    'backgroundPosition','backgroundRepeat','borderBottom','borderTop',
    'borderLeft','borderRight','marginTop','marginBottom','marginLeft',
    'marginRight','paddingTop','paddingBottom','paddingLeft','paddingRight'];

function camelToKebab(str) {
    return str.replace(/[A-Z]/g, function(m) { return '-' + m.toLowerCase(); });
}

cssProps.forEach(function(prop) {
    var kebab = camelToKebab(prop);
    Object.defineProperty(CSSStyleDeclaration.prototype, prop, {
        get: function() { return this._props[kebab] || ''; },
        set: function(v) {
            this._props[kebab] = v;
            this._syncToAttr();
        }
    });
});

// cssText
Object.defineProperty(CSSStyleDeclaration.prototype, 'cssText', {
    get: function() {
        var parts = [];
        for (var k in this._props) {
            if (this._props[k]) parts.push(k + ': ' + this._props[k]);
        }
        return parts.join('; ');
    },
    set: function(v) {
        this._props = {};
        if (v) {
            v.split(';').forEach(function(part) {
                var idx = part.indexOf(':');
                if (idx > 0) {
                    var key = part.substring(0, idx).trim();
                    var val = part.substring(idx + 1).trim();
                    if (key && val) this._props[key] = val;
                }
            }, this);
        }
        this._syncToAttr();
    }
});

// ---------------------------------------------------------------------------
// DOMTokenList (classList)
// ---------------------------------------------------------------------------
function DOMTokenList(elNode) {
    this._el = elNode;
}
DOMTokenList.prototype.add = function() {
    for (var i = 0; i < arguments.length; i++) {
        if (this._el._id) __domSetClass(this._el._id, arguments[i], true);
    }
};
DOMTokenList.prototype.remove = function() {
    for (var i = 0; i < arguments.length; i++) {
        if (this._el._id) __domSetClass(this._el._id, arguments[i], false);
    }
};
DOMTokenList.prototype.contains = function(cls) {
    if (!this._el._id) return false;
    var attr = __domGetAttr(this._el._id, 'class');
    if (!attr) return false;
    return (' ' + attr + ' ').indexOf(' ' + cls + ' ') >= 0;
};
DOMTokenList.prototype.toggle = function(cls, force) {
    if (force !== undefined) {
        if (force) this.add(cls); else this.remove(cls);
        return !!force;
    }
    if (this.contains(cls)) { this.remove(cls); return false; }
    this.add(cls); return true;
};
DOMTokenList.prototype.toString = function() {
    return this._el._id ? (__domGetAttr(this._el._id, 'class') || '') : '';
};
Object.defineProperty(DOMTokenList.prototype, 'length', {
    get: function() {
        var s = this.toString().trim();
        return s ? s.split(/\s+/).length : 0;
    }
});
DOMTokenList.prototype.item = function(i) {
    var parts = this.toString().trim().split(/\s+/);
    return parts[i] || null;
};
DOMTokenList.prototype.forEach = function(cb, thisArg) {
    var parts = this.toString().trim().split(/\s+/);
    parts.forEach(cb, thisArg);
};

// ---------------------------------------------------------------------------
// DOMNode - base class for all DOM nodes
// ---------------------------------------------------------------------------
function DOMNode(id, nodeType) {
    this._id = id;
    this.nodeType = nodeType;
    this._listeners = {};
    this._style = null;
    this._classList = null;
}

// EventTarget methods
DOMNode.prototype.addEventListener = function(type, cb, opts) {
    if (!this._listeners[type]) this._listeners[type] = [];
    this._listeners[type].push(cb);
};
DOMNode.prototype.removeEventListener = function(type, cb) {
    if (!this._listeners[type]) return;
    var idx = this._listeners[type].indexOf(cb);
    if (idx >= 0) this._listeners[type].splice(idx, 1);
};
DOMNode.prototype.dispatchEvent = function(event) {
    var cbs = this._listeners[event.type];
    if (cbs) {
        for (var i = 0; i < cbs.length; i++) {
            try { cbs[i].call(this, event); } catch(e) { console.error(e); }
        }
    }
    return !event.defaultPrevented;
};

// Tree traversal
Object.defineProperty(DOMNode.prototype, 'parentNode', {
    get: function() {
        if (!this._id) return null;
        var pid = __domGetParentId(this._id);
        return pid ? getNode(pid) : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'parentElement', {
    get: function() {
        var p = this.parentNode;
        return (p && p.nodeType === ELEMENT_NODE) ? p : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'childNodes', {
    get: function() {
        if (!this._id) return [];
        var ids = __domGetChildIds(this._id);
        return ids.map(getNode);
    }
});
Object.defineProperty(DOMNode.prototype, 'children', {
    get: function() {
        return this.childNodes.filter(function(n) { return n.nodeType === ELEMENT_NODE; });
    }
});
Object.defineProperty(DOMNode.prototype, 'firstChild', {
    get: function() {
        var cn = this.childNodes;
        return cn.length > 0 ? cn[0] : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'lastChild', {
    get: function() {
        var cn = this.childNodes;
        return cn.length > 0 ? cn[cn.length - 1] : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'nextSibling', {
    get: function() {
        var p = this.parentNode;
        if (!p) return null;
        var cn = p.childNodes;
        for (var i = 0; i < cn.length; i++) {
            if (cn[i]._id === this._id) return cn[i + 1] || null;
        }
        return null;
    }
});
Object.defineProperty(DOMNode.prototype, 'previousSibling', {
    get: function() {
        var p = this.parentNode;
        if (!p) return null;
        var cn = p.childNodes;
        for (var i = 0; i < cn.length; i++) {
            if (cn[i]._id === this._id) return cn[i - 1] || null;
        }
        return null;
    }
});
Object.defineProperty(DOMNode.prototype, 'nextElementSibling', {
    get: function() {
        var s = this.nextSibling;
        while (s && s.nodeType !== ELEMENT_NODE) s = s.nextSibling;
        return s;
    }
});
Object.defineProperty(DOMNode.prototype, 'previousElementSibling', {
    get: function() {
        var s = this.previousSibling;
        while (s && s.nodeType !== ELEMENT_NODE) s = s.previousSibling;
        return s;
    }
});
Object.defineProperty(DOMNode.prototype, 'firstElementChild', {
    get: function() {
        var ch = this.children;
        return ch.length > 0 ? ch[0] : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'lastElementChild', {
    get: function() {
        var ch = this.children;
        return ch.length > 0 ? ch[ch.length - 1] : null;
    }
});
Object.defineProperty(DOMNode.prototype, 'childElementCount', {
    get: function() { return this.children.length; }
});

// Tree manipulation (with MutationObserver notifications)
DOMNode.prototype.appendChild = function(child) {
    if (!this._id || !child || !child._id) return child;
    __domAppendChild(this._id, child._id);
    if (typeof __notifyMutation === 'function') {
        __notifyMutation(this, 'childList', {
            type: 'childList', target: this, addedNodes: [child], removedNodes: []
        });
    }
    return child;
};
DOMNode.prototype.removeChild = function(child) {
    if (!this._id || !child || !child._id) return child;
    __domRemoveChild(this._id, child._id);
    if (typeof __notifyMutation === 'function') {
        __notifyMutation(this, 'childList', {
            type: 'childList', target: this, addedNodes: [], removedNodes: [child]
        });
    }
    return child;
};
DOMNode.prototype.insertBefore = function(newChild, refChild) {
    if (!this._id || !newChild || !newChild._id) return newChild;
    var refId = (refChild && refChild._id) ? refChild._id : 0;
    __domInsertBefore(this._id, newChild._id, refId);
    if (typeof __notifyMutation === 'function') {
        __notifyMutation(this, 'childList', {
            type: 'childList', target: this, addedNodes: [newChild], removedNodes: []
        });
    }
    return newChild;
};
DOMNode.prototype.replaceChild = function(newChild, oldChild) {
    this.insertBefore(newChild, oldChild);
    this.removeChild(oldChild);
    return oldChild;
};
DOMNode.prototype.cloneNode = function(deep) {
    // Create a new element with same tag and attributes
    if (this.nodeType === TEXT_NODE) {
        return document.createTextNode(this.textContent);
    }
    if (this.nodeType === COMMENT_NODE) {
        return document.createComment(this.textContent);
    }
    var clone = document.createElement(this.tagName || 'div');
    // Copy attributes
    if (this._id) {
        var attrs = ['id', 'class', 'style', 'name', 'type', 'value', 'href', 'src'];
        for (var i = 0; i < attrs.length; i++) {
            var v = __domGetAttr(this._id, attrs[i]);
            if (v) __domSetAttr(clone._id, attrs[i], v);
        }
    }
    if (deep) {
        var cn = this.childNodes;
        for (var j = 0; j < cn.length; j++) {
            clone.appendChild(cn[j].cloneNode(true));
        }
    }
    return clone;
};
DOMNode.prototype.contains = function(other) {
    if (!other) return false;
    var node = other;
    while (node) {
        if (node._id === this._id) return true;
        node = node.parentNode;
    }
    return false;
};
DOMNode.prototype.hasChildNodes = function() {
    return this.childNodes.length > 0;
};
DOMNode.prototype.isEqualNode = function(other) {
    return other && this._id === other._id;
};
DOMNode.prototype.isSameNode = function(other) {
    return other && this._id === other._id;
};
Object.defineProperty(DOMNode.prototype, 'textContent', {
    get: function() {
        return this._id ? __domGetText(this._id) : '';
    },
    set: function(v) {
        if (this._id) {
            __domSetText(this._id, v || '');
            if (typeof __notifyMutation === 'function') {
                __notifyMutation(this, 'characterData', {
                    type: 'characterData', target: this
                });
            }
        }
    }
});
Object.defineProperty(DOMNode.prototype, 'ownerDocument', {
    get: function() { return document; }
});
Object.defineProperty(DOMNode.prototype, 'isConnected', {
    get: function() {
        var node = this;
        while (node) {
            if (node === document.documentElement) return true;
            node = node.parentNode;
        }
        return false;
    }
});

// ---------------------------------------------------------------------------
// Element node (nodeType = 1)
// ---------------------------------------------------------------------------
function DOMElement(id) {
    DOMNode.call(this, id, ELEMENT_NODE);
}
DOMElement.prototype = Object.create(DOMNode.prototype);
DOMElement.prototype.constructor = DOMElement;

Object.defineProperty(DOMElement.prototype, 'tagName', {
    get: function() {
        return this._id ? __domGetTagName(this._id).toUpperCase() : '';
    }
});
Object.defineProperty(DOMElement.prototype, 'nodeName', {
    get: function() { return this.tagName; }
});
Object.defineProperty(DOMElement.prototype, 'localName', {
    get: function() { return this.tagName.toLowerCase(); }
});
Object.defineProperty(DOMElement.prototype, 'id', {
    get: function() { return this._id ? (__domGetAttr(this._id, 'id') || '') : ''; },
    set: function(v) { if (this._id) __domSetAttr(this._id, 'id', v); }
});
Object.defineProperty(DOMElement.prototype, 'className', {
    get: function() { return this._id ? (__domGetAttr(this._id, 'class') || '') : ''; },
    set: function(v) { if (this._id) __domSetAttr(this._id, 'class', v); }
});
Object.defineProperty(DOMElement.prototype, 'classList', {
    get: function() {
        if (!this._classList) this._classList = new DOMTokenList(this);
        return this._classList;
    }
});
Object.defineProperty(DOMElement.prototype, 'style', {
    get: function() {
        if (!this._style) this._style = new CSSStyleDeclaration(this);
        return this._style;
    }
});
Object.defineProperty(DOMElement.prototype, 'innerHTML', {
    get: function() {
        // Simplified: return text content
        return this._id ? __domGetText(this._id) : '';
    },
    set: function(v) {
        if (this._id) {
            __domSetInnerHTML(this._id, v || '');
            if (typeof __notifyMutation === 'function') {
                __notifyMutation(this, 'childList', {
                    type: 'childList', target: this, addedNodes: [], removedNodes: []
                });
            }
        }
    }
});
Object.defineProperty(DOMElement.prototype, 'outerHTML', {
    get: function() {
        return '<' + this.tagName.toLowerCase() + '>' + this.innerHTML +
               '</' + this.tagName.toLowerCase() + '>';
    }
});
Object.defineProperty(DOMElement.prototype, 'innerText', {
    get: function() { return this.textContent; },
    set: function(v) { this.textContent = v; }
});

// Attribute methods
DOMElement.prototype.getAttribute = function(name) {
    return this._id ? __domGetAttr(this._id, name) : null;
};
DOMElement.prototype.setAttribute = function(name, value) {
    if (this._id) {
        var oldVal = __domGetAttr(this._id, name);
        __domSetAttr(this._id, name, String(value));
        if (typeof __notifyMutation === 'function') {
            __notifyMutation(this, 'attributes', {
                type: 'attributes', target: this, attributeName: name, oldValue: oldVal
            });
        }
    }
};
DOMElement.prototype.removeAttribute = function(name) {
    if (this._id) __domRemoveAttr(this._id, name);
};
DOMElement.prototype.hasAttribute = function(name) {
    return this.getAttribute(name) !== null;
};
DOMElement.prototype.getAttributeNS = function(ns, name) {
    return this.getAttribute(name);
};
DOMElement.prototype.setAttributeNS = function(ns, name, value) {
    this.setAttribute(name, value);
};
DOMElement.prototype.removeAttributeNS = function(ns, name) {
    this.removeAttribute(name);
};

// Query methods
DOMElement.prototype.querySelector = function(selector) {
    if (!this._id) return null;
    var id = __domQuerySelector(this._id, selector);
    return id ? getNode(id) : null;
};
DOMElement.prototype.querySelectorAll = function(selector) {
    if (!this._id) return [];
    var ids = __domQuerySelectorAll(this._id, selector);
    var result = ids.map(getNode);
    result.item = function(i) { return this[i] || null; };
    return result;
};
DOMElement.prototype.getElementsByTagName = function(tag) {
    return this.querySelectorAll(tag);
};
DOMElement.prototype.getElementsByClassName = function(cls) {
    return this.querySelectorAll('.' + cls);
};
DOMElement.prototype.matches = function(selector) {
    // Use querySelectorAll on parent and check if this is in results
    var p = this.parentNode;
    if (!p || !p._id) return false;
    var ids = __domQuerySelectorAll(p._id, selector);
    return ids.indexOf(this._id) >= 0;
};
DOMElement.prototype.closest = function(selector) {
    var node = this;
    while (node && node.nodeType === ELEMENT_NODE) {
        if (node.matches(selector)) return node;
        node = node.parentNode;
    }
    return null;
};

// Layout stubs
DOMElement.prototype.getBoundingClientRect = function() {
    return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0 };
};
DOMElement.prototype.getClientRects = function() {
    return [this.getBoundingClientRect()];
};
Object.defineProperty(DOMElement.prototype, 'offsetWidth', { get: function() { return 0; } });
Object.defineProperty(DOMElement.prototype, 'offsetHeight', { get: function() { return 0; } });
Object.defineProperty(DOMElement.prototype, 'offsetTop', { get: function() { return 0; } });
Object.defineProperty(DOMElement.prototype, 'offsetLeft', { get: function() { return 0; } });
Object.defineProperty(DOMElement.prototype, 'clientWidth', {
    get: function() {
        var tag = this.tagName;
        if (tag === 'HTML' || tag === 'BODY') return innerWidth || 640;
        return 0;
    }
});
Object.defineProperty(DOMElement.prototype, 'clientHeight', {
    get: function() {
        var tag = this.tagName;
        if (tag === 'HTML' || tag === 'BODY') return innerHeight || 480;
        return 0;
    }
});
Object.defineProperty(DOMElement.prototype, 'scrollWidth', { get: function() { return this.clientWidth; } });
Object.defineProperty(DOMElement.prototype, 'scrollHeight', { get: function() { return this.clientHeight; } });
DOMElement.prototype.scrollTop = 0;
DOMElement.prototype.scrollLeft = 0;
DOMElement.prototype.clientTop = 0;
DOMElement.prototype.clientLeft = 0;

// Misc
DOMElement.prototype.focus = function() {};
DOMElement.prototype.blur = function() {};
DOMElement.prototype.click = function() {
    this.dispatchEvent(new Event('click'));
};
DOMElement.prototype.remove = function() {
    var p = this.parentNode;
    if (p) p.removeChild(this);
};
DOMElement.prototype.insertAdjacentElement = function(position, el) {
    switch (position) {
        case 'beforebegin': this.parentNode && this.parentNode.insertBefore(el, this); break;
        case 'afterbegin': this.insertBefore(el, this.firstChild); break;
        case 'beforeend': this.appendChild(el); break;
        case 'afterend': this.parentNode && this.parentNode.insertBefore(el, this.nextSibling); break;
    }
    return el;
};
DOMElement.prototype.insertAdjacentHTML = function(position, html) {
    // Simplified: create element, set innerHTML, insert
    var temp = document.createElement('div');
    temp.innerHTML = html;
    var children = temp.childNodes;
    for (var i = 0; i < children.length; i++) {
        this.insertAdjacentElement(position, children[i]);
    }
};

// Canvas-specific: getContext (for Phaser)
DOMElement.prototype.getContext = function(type) {
    if (this.tagName === 'CANVAS') {
        if (type === '2d') {
            // Return SoftCanvas2D for 2D context (used by Phaser for text rendering)
            if (!this._ctx2d && typeof SoftCanvas2D === 'function') {
                this._ctx2d = new SoftCanvas2D(this);
            }
            return this._ctx2d || null;
        }
        // Delegate to existing canvas/WebGL shim
        if (type === 'webgl' || type === 'webgl2' || type === 'experimental-webgl') {
            return typeof __getWebGLContext === 'function' ? __getWebGLContext(this) : null;
        }
    }
    return null;
};

// Dataset (data-* attributes)
Object.defineProperty(DOMElement.prototype, 'dataset', {
    get: function() {
        var self = this;
        return new Proxy({}, {
            get: function(target, prop) {
                return self.getAttribute('data-' + prop.replace(/[A-Z]/g, function(m) { return '-' + m.toLowerCase(); }));
            },
            set: function(target, prop, value) {
                self.setAttribute('data-' + prop.replace(/[A-Z]/g, function(m) { return '-' + m.toLowerCase(); }), value);
                return true;
            }
        });
    }
});

// ---------------------------------------------------------------------------
// Text node (nodeType = 3)
// ---------------------------------------------------------------------------
function DOMText(id) {
    DOMNode.call(this, id, TEXT_NODE);
}
DOMText.prototype = Object.create(DOMNode.prototype);
DOMText.prototype.constructor = DOMText;
Object.defineProperty(DOMText.prototype, 'nodeName', { get: function() { return '#text'; } });
Object.defineProperty(DOMText.prototype, 'tagName', { get: function() { return undefined; } });
Object.defineProperty(DOMText.prototype, 'data', {
    get: function() { return this.textContent; },
    set: function(v) { this.textContent = v; }
});
Object.defineProperty(DOMText.prototype, 'nodeValue', {
    get: function() { return this.textContent; },
    set: function(v) { this.textContent = v; }
});
Object.defineProperty(DOMText.prototype, 'wholeText', {
    get: function() { return this.textContent; }
});
DOMText.prototype.splitText = function() { return this; };

// ---------------------------------------------------------------------------
// Comment node (nodeType = 8)
// ---------------------------------------------------------------------------
function DOMComment(id) {
    DOMNode.call(this, id, COMMENT_NODE);
}
DOMComment.prototype = Object.create(DOMNode.prototype);
DOMComment.prototype.constructor = DOMComment;
Object.defineProperty(DOMComment.prototype, 'nodeName', { get: function() { return '#comment'; } });
Object.defineProperty(DOMComment.prototype, 'data', {
    get: function() { return this.textContent; },
    set: function(v) { this.textContent = v; }
});

// ---------------------------------------------------------------------------
// DocumentFragment (nodeType = 11)
// Uses a virtual litehtml element as container
// ---------------------------------------------------------------------------
function DOMDocumentFragment() {
    var id = __domCreateElement('div'); // use a div as backing element
    DOMNode.call(this, id, DOCUMENT_FRAGMENT_NODE);
}
DOMDocumentFragment.prototype = Object.create(DOMNode.prototype);
DOMDocumentFragment.prototype.constructor = DOMDocumentFragment;
Object.defineProperty(DOMDocumentFragment.prototype, 'nodeName', { get: function() { return '#document-fragment'; } });
DOMDocumentFragment.prototype.querySelector = DOMElement.prototype.querySelector;
DOMDocumentFragment.prototype.querySelectorAll = DOMElement.prototype.querySelectorAll;
DOMDocumentFragment.prototype.getElementById = function(id) {
    return this.querySelector('#' + id);
};

// ---------------------------------------------------------------------------
// Node factory: get or create a JS wrapper for a litehtml element ID
// ---------------------------------------------------------------------------
function getNode(id) {
    if (!id) return null;
    if (nodeCache[id]) return nodeCache[id];

    var node;
    if (__domIsText(id)) {
        node = new DOMText(id);
    } else if (__domIsComment(id)) {
        node = new DOMComment(id);
    } else {
        node = new DOMElement(id);
    }
    nodeCache[id] = node;
    return node;
}

// ---------------------------------------------------------------------------
// Document object
// ---------------------------------------------------------------------------

// Find key elements from the parsed HTML tree
var docRootId = __domGetDocRootId();
var htmlEl = docRootId ? getNode(docRootId) : null;
var bodyEl = htmlEl ? htmlEl.querySelector('body') : null;
var headEl = htmlEl ? htmlEl.querySelector('head') : null;

// Create missing elements if needed (DOM bridge may not be loaded for plain JS files)
if (!htmlEl) {
    var hid = __domCreateElement('html');
    htmlEl = hid ? getNode(hid) : null;
}
if (htmlEl && !bodyEl) {
    var bid = __domCreateElement('body');
    bodyEl = bid ? getNode(bid) : null;
    if (bodyEl) htmlEl.appendChild(bodyEl);
}
if (htmlEl && !headEl) {
    var heid = __domCreateElement('head');
    headEl = heid ? getNode(heid) : null;
    if (headEl) htmlEl.insertBefore(headEl, bodyEl);
}

// Enhance the existing document object or create a new one
var doc = (typeof document !== 'undefined') ? document : {};

doc.nodeType = DOCUMENT_NODE;
doc.nodeName = '#document';
doc.documentElement = htmlEl;
doc.body = bodyEl;
doc.head = headEl;
doc.defaultView = (typeof window !== 'undefined') ? window : null;
doc.readyState = 'complete';
doc.visibilityState = 'visible';
doc.hidden = false;

doc.createElement = function(tag) {
    var tagLower = tag.toLowerCase();
    // Canvas: return the shim canvas with a backing DOM element for tree ops
    if (tagLower === 'canvas' && typeof __createCanvas === 'function') {
        var canvas = __createCanvas();
        if (canvas) {
            // Give it a backing litehtml element so appendChild/removeChild work
            var backingId = __domCreateElement('canvas');
            canvas._id = backingId;
            nodeCache[backingId] = canvas;
            canvas.nodeType = ELEMENT_NODE;
            canvas.nodeName = 'CANVAS';
            canvas.tagName = 'CANVAS';
            canvas.ownerDocument = doc;
            canvas._listeners = canvas._listeners || {};
            // DOM tree traversal via backing element
            Object.defineProperty(canvas, 'parentNode', {
                get: function() {
                    if (!this._id) return null;
                    var pid = __domGetParentId(this._id);
                    return pid ? getNode(pid) : null;
                }, configurable: true
            });
            Object.defineProperty(canvas, 'parentElement', {
                get: function() { var p = this.parentNode; return (p && p.nodeType === ELEMENT_NODE) ? p : null; },
                configurable: true
            });
            canvas.appendChild = DOMNode.prototype.appendChild;
            canvas.removeChild = DOMNode.prototype.removeChild;
            canvas.insertBefore = DOMNode.prototype.insertBefore;
            canvas.contains = DOMNode.prototype.contains;
            canvas.dispatchEvent = DOMNode.prototype.dispatchEvent;
            if (!canvas.querySelector) canvas.querySelector = function(){ return null; };
            if (!canvas.querySelectorAll) canvas.querySelectorAll = function(){ return []; };
            if (!canvas.matches) canvas.matches = function(){ return false; };
            if (!canvas.closest) canvas.closest = function(){ return null; };
            if (!canvas.hasAttribute) canvas.hasAttribute = function(){ return false; };
            canvas.setAttribute = function(name, val) { if (this._id) __domSetAttr(this._id, name, String(val)); };
            canvas.getAttribute = function(name) { return this._id ? __domGetAttr(this._id, name) : null; };
            canvas.remove = function() {
                var p = this.parentNode;
                if (p) p.removeChild(this);
            };
            return canvas;
        }
    }
    // Audio: return the shim audio element
    if (tagLower === 'audio' && typeof __createStubElement === 'function') {
        return __createStubElement('audio');
    }
    var id = __domCreateElement(tag);
    return getNode(id);
};

doc.createTextNode = function(text) {
    var id = __domCreateTextNode(text || '');
    return getNode(id);
};

doc.createComment = function(text) {
    // Use text node as backing, wrap as comment
    var id = __domCreateTextNode(text || '');
    var node = new DOMComment(id);
    nodeCache[id] = node;
    return node;
};

doc.createDocumentFragment = function() {
    return new DOMDocumentFragment();
};

doc.createElementNS = function(ns, tag) {
    return doc.createElement(tag);
};

doc.getElementById = function(id) {
    return doc.querySelector('#' + id);
};

doc.querySelector = function(selector) {
    if (!selector) return null;
    // For 'canvas' selector, check if we have a canvas in our shim
    var id = __domQuerySelector(0, selector);
    return id ? getNode(id) : null;
};

doc.querySelectorAll = function(selector) {
    if (!selector) return [];
    var ids = __domQuerySelectorAll(0, selector);
    var result = ids.map(getNode);
    result.item = function(i) { return this[i] || null; };
    return result;
};

doc.getElementsByTagName = function(tag) {
    return doc.querySelectorAll(tag);
};

doc.getElementsByClassName = function(cls) {
    return doc.querySelectorAll('.' + cls);
};

// Event methods on document
doc._listeners = {};
doc.addEventListener = function(type, cb, opts) {
    if (!doc._listeners[type]) doc._listeners[type] = [];
    doc._listeners[type].push(cb);
};
doc.removeEventListener = function(type, cb) {
    if (!doc._listeners[type]) return;
    var idx = doc._listeners[type].indexOf(cb);
    if (idx >= 0) doc._listeners[type].splice(idx, 1);
};
doc.dispatchEvent = function(event) {
    var cbs = doc._listeners[event.type];
    if (cbs) {
        for (var i = 0; i < cbs.length; i++) {
            try { cbs[i].call(doc, event); } catch(e) { console.error(e); }
        }
    }
    return true;
};

// Stub methods
doc.createRange = function() {
    return {
        selectNodeContents: function() {},
        collapse: function() {},
        getBoundingClientRect: function() { return { x:0, y:0, width:0, height:0, top:0, left:0, bottom:0, right:0 }; },
        getClientRects: function() { return []; },
        setStart: function() {},
        setEnd: function() {},
        commonAncestorContainer: bodyEl,
        startContainer: bodyEl,
        endContainer: bodyEl,
        startOffset: 0,
        endOffset: 0,
        collapsed: true,
        createContextualFragment: function(html) {
            var frag = doc.createDocumentFragment();
            frag.innerHTML = html;
            return frag;
        }
    };
};

doc.createTreeWalker = function(root) {
    var nodes = root ? [root] : [];
    var idx = 0;
    return {
        currentNode: root,
        nextNode: function() {
            idx++;
            this.currentNode = nodes[idx] || null;
            return this.currentNode;
        }
    };
};

doc.getSelection = function() {
    return { rangeCount: 0, addRange: function() {}, removeAllRanges: function() {} };
};

doc.hasFocus = function() { return true; };

doc.execCommand = function() { return false; };

doc.adoptNode = function(node) { return node; };
doc.importNode = function(node, deep) { return node.cloneNode(deep); };

// document.body convenience methods
if (bodyEl) {
    bodyEl.getBoundingClientRect = function() {
        return { left:0, top:0, right: innerWidth||640, bottom: innerHeight||480,
                 width: innerWidth||640, height: innerHeight||480, x:0, y:0 };
    };
}

// window.getComputedStyle
if (typeof window !== 'undefined') {
    window.getComputedStyle = function(el) {
        return el.style || new CSSStyleDeclaration(el);
    };
}

// Make document global
if (typeof window !== 'undefined') {
    window.document = doc;
}

// Export node types
window.Node = DOMNode;
window.Element = DOMElement;
window.Text = DOMText;
window.Comment = DOMComment;
window.DocumentFragment = DOMDocumentFragment;
window.HTMLElement = DOMElement;
window.HTMLDivElement = DOMElement;
window.HTMLSpanElement = DOMElement;
window.HTMLCanvasElement = DOMElement;
window.HTMLImageElement = DOMElement;
window.HTMLAudioElement = DOMElement;
window.HTMLVideoElement = DOMElement;
window.HTMLInputElement = DOMElement;
window.HTMLButtonElement = DOMElement;
window.HTMLAnchorElement = DOMElement;
window.HTMLFormElement = DOMElement;
window.HTMLSelectElement = DOMElement;
window.HTMLTextAreaElement = DOMElement;
window.HTMLStyleElement = DOMElement;
window.HTMLLinkElement = DOMElement;
window.HTMLScriptElement = DOMElement;
window.HTMLHeadElement = DOMElement;
window.HTMLBodyElement = DOMElement;
window.HTMLHtmlElement = DOMElement;
window.HTMLDocument = function() {};
window.Document = function() {};
window.CSSStyleDeclaration = CSSStyleDeclaration;

// Node type constants
DOMNode.ELEMENT_NODE = ELEMENT_NODE;
DOMNode.TEXT_NODE = TEXT_NODE;
DOMNode.COMMENT_NODE = COMMENT_NODE;
DOMNode.DOCUMENT_NODE = DOCUMENT_NODE;
DOMNode.DOCUMENT_FRAGMENT_NODE = DOCUMENT_FRAGMENT_NODE;

// Event class (basic)
if (typeof Event === 'undefined' || !Event.prototype) {
    window.Event = function(type, opts) {
        this.type = type;
        this.bubbles = opts && opts.bubbles || false;
        this.cancelable = opts && opts.cancelable || false;
        this.defaultPrevented = false;
        this.target = null;
        this.currentTarget = null;
        this.timeStamp = Date.now();
    };
    window.Event.prototype.preventDefault = function() { this.defaultPrevented = true; };
    window.Event.prototype.stopPropagation = function() {};
    window.Event.prototype.stopImmediatePropagation = function() {};
}

if (typeof CustomEvent === 'undefined') {
    window.CustomEvent = function(type, opts) {
        Event.call(this, type, opts);
        this.detail = opts && opts.detail || null;
    };
    window.CustomEvent.prototype = Object.create(Event.prototype);
}

// MutationObserver - functional implementation that Angular needs
// Tracks observed targets and fires callbacks when DOM tree is mutated
(function() {
    var observers = [];

    function MutationObserverImpl(callback) {
        this._callback = callback;
        this._targets = [];
        this._pending = [];
        this._scheduled = false;
    }
    MutationObserverImpl.prototype.observe = function(target, options) {
        if (!target) return;
        this._targets.push({ target: target, options: options || {} });
        if (observers.indexOf(this) < 0) observers.push(this);
    };
    MutationObserverImpl.prototype.disconnect = function() {
        this._targets = [];
        var idx = observers.indexOf(this);
        if (idx >= 0) observers.splice(idx, 1);
    };
    MutationObserverImpl.prototype.takeRecords = function() {
        var records = this._pending;
        this._pending = [];
        return records;
    };
    MutationObserverImpl.prototype._notify = function(record) {
        this._pending.push(record);
        if (!this._scheduled) {
            this._scheduled = true;
            var self = this;
            queueMicrotask(function() {
                self._scheduled = false;
                var records = self._pending;
                self._pending = [];
                if (records.length > 0) {
                    try { self._callback(records, self); } catch(e) { console.error('[MutationObserver]', e.message || e); }
                }
            });
        }
    };

    // Notify all observers watching a given target about a mutation
    window.__notifyMutation = function(target, type, record) {
        for (var i = 0; i < observers.length; i++) {
            var obs = observers[i];
            for (var j = 0; j < obs._targets.length; j++) {
                var t = obs._targets[j];
                var watching = (t.target === target);
                // subtree: also match if target is a descendant
                if (!watching && t.options.subtree) {
                    var node = target;
                    while (node) {
                        if (node === t.target) { watching = true; break; }
                        node = node.parentNode;
                    }
                }
                if (watching) {
                    if (type === 'childList' && t.options.childList) {
                        obs._notify(record);
                    } else if (type === 'attributes' && t.options.attributes) {
                        obs._notify(record);
                    } else if (type === 'characterData' && t.options.characterData) {
                        obs._notify(record);
                    }
                    break;
                }
            }
        }
    };

    window.MutationObserver = MutationObserverImpl;
})();

// ResizeObserver stub
if (typeof ResizeObserver === 'undefined') {
    window.ResizeObserver = function(callback) {
        this._callback = callback;
    };
    window.ResizeObserver.prototype.observe = function() {};
    window.ResizeObserver.prototype.unobserve = function() {};
    window.ResizeObserver.prototype.disconnect = function() {};
}

// IntersectionObserver stub
if (typeof IntersectionObserver === 'undefined') {
    window.IntersectionObserver = function(callback) {
        this._callback = callback;
    };
    window.IntersectionObserver.prototype.observe = function() {};
    window.IntersectionObserver.prototype.unobserve = function() {};
    window.IntersectionObserver.prototype.disconnect = function() {};
}

console.log('[DOM] litehtml-backed DOM initialized (html=' + (htmlEl ? htmlEl._id : 'null') +
    ', body=' + (bodyEl ? bodyEl._id : 'null') +
    ', head=' + (headEl ? headEl._id : 'null') + ')');

})();
