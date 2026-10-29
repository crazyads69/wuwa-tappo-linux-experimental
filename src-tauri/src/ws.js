// Redirects the multiplayer socket (and nothing else) to the native client in native_ws.rs.
// Kuro's socket server rejects the Origin WebKitGTK sends (tauri://localhost) with HTTP 403.
// Injected into every window, like cookies.js. Any other WebSocket URL still uses the browser's.
(() => {
  const I = window.__TAURI_INTERNALS__;
  const Native = window.WebSocket;
  if (!I || !Native || window.__wwWebSocket) return;
  window.__wwWebSocket = true;

  const HOST = 'api-wutheringwaves-wuwatappo.kurogames-global.com';
  const matches = (u) => u.protocol === 'wss:' && u.hostname === HOST && !u.port && (u.pathname === '/socket.io/' || u.pathname === '/socket.io');
  const viewId = Math.random().toString(36).slice(2);
  let counter = 0;

  const toBase64 = (bytes) => {
    let s = '';
    for (let i = 0; i < bytes.length; i += 0x8000) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 0x8000));
    return btoa(s);
  };
  const fromBase64 = (b64) => Uint8Array.from(atob(b64), (c) => c.charCodeAt(0));
  const bytesOf = async (data) => {
    if (data instanceof ArrayBuffer) return new Uint8Array(data);
    if (ArrayBuffer.isView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
    if (typeof Blob !== 'undefined' && data instanceof Blob) return new Uint8Array(await data.arrayBuffer());
    return null;
  };

  class WWWebSocket extends EventTarget {
    static CONNECTING = 0;
    static OPEN = 1;
    static CLOSING = 2;
    static CLOSED = 3;
    #state = 0;
    #binaryType = 'blob';
    #protocol = '';
    #extensions = '';
    #url;
    #id = `ws-${viewId}-${++counter}`;
    #callback;
    #chain; // sends and close are strictly ordered after the connect call
    #handlers = {};

    constructor(url, protocols) {
      super();
      this.#url = url;
      const list = protocols === undefined ? [] : typeof protocols === 'string' ? [protocols] : Array.from(protocols, String);
      // Tauri channel: messages arrive as {index, message} or {index, end}, possibly out of order.
      let next = 0;
      const pending = new Map();
      this.#callback = I.transformCallback((raw) => {
        if ('end' in raw) return;
        if (raw.index === next) {
          this.#handle(raw.message);
          next += 1;
          while (pending.has(next)) { this.#handle(pending.get(next)); pending.delete(next); next += 1; }
        } else pending.set(raw.index, raw.message);
      });
      this.#chain = Promise.resolve()
        .then(() => I.invoke('native_ws_connect', { id: this.#id, url, protocols: list, events: `__CHANNEL__:${this.#callback}` }))
        .catch(() => this.#fail());
    }

    get CONNECTING() { return 0; }
    get OPEN() { return 1; }
    get CLOSING() { return 2; }
    get CLOSED() { return 3; }
    get readyState() { return this.#state; }
    get url() { return this.#url; }
    get protocol() { return this.#protocol; }
    get extensions() { return this.#extensions; }
    get bufferedAmount() { return 0; }
    get binaryType() { return this.#binaryType; }
    set binaryType(v) { if (v === 'blob' || v === 'arraybuffer') this.#binaryType = v; }
    get onopen() { return this.#handlers.open ?? null; }
    set onopen(f) { this.#handlers.open = f; }
    get onmessage() { return this.#handlers.message ?? null; }
    set onmessage(f) { this.#handlers.message = f; }
    get onclose() { return this.#handlers.close ?? null; }
    set onclose(f) { this.#handlers.close = f; }
    get onerror() { return this.#handlers.error ?? null; }
    set onerror(f) { this.#handlers.error = f; }

    #fire(event) {
      this.dispatchEvent(event);
      const handler = this.#handlers[event.type];
      if (typeof handler === 'function') handler.call(this, event);
    }

    #finish(code, reason, clean) {
      if (this.#state === 3) return;
      this.#state = 3;
      this.#fire(new CloseEvent('close', { code, reason, wasClean: clean }));
      try { I.unregisterCallback?.(this.#callback); } catch (_) {}
    }

    #fail() {
      if (this.#state === 3) return;
      this.#fire(new Event('error'));
      this.#finish(1006, '', false);
    }

    #handle(m) {
      switch (m.event) {
        case 'open':
          if (this.#state !== 0) return;
          this.#state = 1;
          this.#protocol = m.protocol || '';
          this.#extensions = m.extensions || '';
          this.#fire(new Event('open'));
          break;
        case 'message': {
          if (this.#state !== 1 && this.#state !== 2) return;
          let data = m.data;
          if (m.kind === 'binary') {
            const bytes = fromBase64(m.data);
            data = this.#binaryType === 'arraybuffer' ? bytes.buffer : new Blob([bytes]);
          }
          this.#fire(new MessageEvent('message', { data }));
          break;
        }
        case 'error':
          this.#fire(new Event('error'));
          break;
        case 'close':
          this.#finish(m.code, m.reason || '', !!m.clean);
          break;
      }
    }

    send(data) {
      if (this.#state === 0) throw new DOMException('WebSocket is still in CONNECTING state.', 'InvalidStateError');
      if (this.#state !== 1) return;
      this.#chain = this.#chain
        .then(async () => {
          const bytes = typeof data === 'string' ? null : await bytesOf(data);
          if (bytes) return I.invoke('native_ws_send', { id: this.#id, kind: 'binary', data: toBase64(bytes) });
          return I.invoke('native_ws_send', { id: this.#id, kind: 'text', data: String(data) });
        })
        .catch(() => this.#fail());
    }

    close(code, reason = '') {
      if (code !== undefined && code !== 1000 && !(code >= 3000 && code <= 4999)) throw new DOMException('Invalid close code', 'InvalidAccessError');
      if (new TextEncoder().encode(String(reason)).length > 123) throw new DOMException('Close reason too long', 'SyntaxError');
      if (this.#state >= 2) return;
      this.#state = 2;
      this.#chain = this.#chain.then(() => I.invoke('native_ws_close', { id: this.#id, code: code ?? null, reason: String(reason) })).catch(() => this.#fail());
    }
  }

  const Wrapped = function WebSocket(url, protocols) {
    let target = null;
    try { target = new URL(String(url), location.href); } catch (_) { /* the browser reports the error */ }
    if (target && matches(target)) return new WWWebSocket(target.href, protocols);
    return protocols === undefined ? new Native(url) : new Native(url, protocols);
  };
  Wrapped.prototype = Native.prototype;
  for (const [k, v] of Object.entries({ CONNECTING: 0, OPEN: 1, CLOSING: 2, CLOSED: 3 })) Wrapped[k] = v;
  Object.defineProperty(window, 'WebSocket', { value: Wrapped, writable: true, configurable: true });
})();
