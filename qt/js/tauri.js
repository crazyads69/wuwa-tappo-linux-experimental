// Minimal Tauri 2 runtime for the unmodified frontend: window.__TAURI_INTERNALS__ over a QWebChannel to the C++
// `backend` object. Injected into every window before the page's scripts (see qml/Page.qml).
(() => {
  if (window.__TAURI_INTERNALS__) return;
  const label = window.__WW_LABEL__ || 'main';
  let backend = null;
  const queue = [];
  const pending = new Map();
  const listeners = new Map(); // eventId -> { event, handler }
  let seq = 0, evSeq = 0;

  const transformCallback = (cb, once = false) => {
    const id = crypto.getRandomValues(new Uint32Array(1))[0];
    Object.defineProperty(window, `_${id}`, {
      value: (r) => { if (once) Reflect.deleteProperty(window, `_${id}`); return cb && cb(r); },
      writable: false, configurable: true,
    });
    return id;
  };
  const unregisterCallback = (id) => { Reflect.deleteProperty(window, `_${id}`); };

  const dispatch = (name, payload) => {
    if (window.__WW_DEBUG__) console.log('[tauri] event', name);
    if (name === '__ww_regions') { window.__wwRegions = payload; return; } // pet hit regions (memberId routing)
    for (const [id, l] of listeners) if (l.event === name) window[`_${l.handler}`]?.({ event: name, id, payload });
  };

  const connect = () => {
    if (!window.qt?.webChannelTransport) return setTimeout(connect, 5);
    new QWebChannel(qt.webChannelTransport, (ch) => {
      backend = ch.objects.backend;
      backend.replied.connect((l, id, ok, json) => {
        if (l !== label) return;
        const p = pending.get(id);
        if (!p) return;
        pending.delete(id);
        const v = json === '' ? null : JSON.parse(json);
        ok ? p.resolve(v) : p.reject(v);
      });
      backend.pageEvent.connect((l, name, json) => {
        if (l === '' || l === label) dispatch(name, json === '' ? null : JSON.parse(json));
      });
      backend.channelMsg.connect((l, cb, index, json) => { if (l === label) window[`_${cb}`]?.({ index, message: JSON.parse(json) }); });
      for (const f of queue.splice(0)) f();
    });
  };
  connect();

  const send = (cmd, args) => new Promise((resolve, reject) => {
    const id = ++seq;
    pending.set(id, { resolve, reject });
    const go = () => backend.invoke(label, id, cmd, JSON.stringify(args ?? {}));
    backend ? go() : queue.push(go);
  });

  const invoke = (cmd, args = {}, _options) => {
    switch (cmd) {
      case 'plugin:event|listen': {
        const id = ++evSeq;
        if (window.__WW_DEBUG__) console.log('[tauri] listen', args.event);
        listeners.set(id, { event: args.event, handler: args.handler });
        return Promise.resolve(id);
      }
      case 'plugin:event|unlisten': listeners.delete(args.eventId); return Promise.resolve();
    }
    return send(cmd, args);
  };

  window.__TAURI_EVENT_PLUGIN_INTERNALS__ = {
    unregisterListener(_event, eventId) {
      const l = listeners.get(eventId);
      if (l) { unregisterCallback(l.handler); listeners.delete(eventId); }
    },
  };
  window.isTauri = true;
  window.__TAURI_INTERNALS__ = {
    invoke, transformCallback, unregisterCallback,
    convertFileSrc: (path, protocol = 'asset') => `${protocol}://localhost/${encodeURIComponent(path)}`,
    metadata: {
      currentWindow: { label },
      currentWebview: { windowLabel: label, label },
    },
  };
})();
