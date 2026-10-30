// Main window only. The window is click-through except over the hit regions, so every pointer
// event that arrives here is meant for the pet. Re-emit it as the native "mouse-*" events the
// frontend expects (physical screen px) and swallow the DOM event so nothing is handled twice.
(() => {
  const I = window.__TAURI_INTERNALS__;
  if (!I || window.__wwShim) return;
  window.__wwShim = true;
  const label = I.metadata.currentWindow.label;
  const call = (cmd, args) => (window.__wwInvoke || I.invoke)(cmd, { label, ...args });
  const emit = (event, payload) => I.invoke('plugin:event|emit', { event, payload }).catch(() => {});
  let inner = { x: 0, y: 0 }, sf = window.devicePixelRatio || 1;
  let originRevision = 0;
  const originChanged = I.transformCallback(({ payload }) => {
    if (!Number.isFinite(payload?.origin?.x) || !Number.isFinite(payload?.origin?.y)
      || !Number.isFinite(payload?.monitor?.scaleFactor) || payload.monitor.scaleFactor <= 0) return;
    originRevision += 1;
    inner = payload.origin;
    sf = payload.monitor.scaleFactor;
  });
  I.invoke('plugin:event|listen', {
    event: 'cat-main-monitor-origin-changed', target: { kind: 'Any' }, handler: originChanged,
  }).catch(() => {});
  const refresh = async () => {
    const revision = originRevision;
    try {
      const p = await call('plugin:window|inner_position');
      const scale = await call('plugin:window|scale_factor');
      if (revision === originRevision) {
        inner = { x: p.x, y: p.y };
        sf = scale;
      }
    } catch (_) {}
  };
  refresh();
  setInterval(refresh, 1500);
  addEventListener('resize', refresh);

  const pos = (e) => {
    const p = { x: Math.round(e.clientX * sf + inner.x), y: Math.round(e.clientY * sf + inner.y) };
    for (const r of window.__wwRegions || []) {
      if (e.clientX >= r.left && e.clientX <= r.left + r.width && e.clientY >= r.top && e.clientY <= r.top + r.height) {
        p.memberId = r.memberId;
        break;
      }
    }
    return p;
  };
  const btn = ['left', 'middle', 'right'];
  let moveQueued = null;
  // Ic() in the recovered frontend dispatches synthetic button clicks after native hit-testing.
  const swallow = (e) => { if (e.isTrusted) { e.stopImmediatePropagation(); e.preventDefault(); } };

  addEventListener('pointermove', (e) => {
    if (!e.isTrusted) return;
    swallow(e);
    if (moveQueued) { moveQueued = e; return; }
    moveQueued = e;
    requestAnimationFrame(() => { emit('mouse-move', pos(moveQueued)); moveQueued = null; });
  }, true);
  for (const [dom, action] of [['pointerdown', 'press'], ['pointerup', 'release']]) {
    addEventListener(dom, (e) => {
      if (!e.isTrusted) return;
      swallow(e);
      // The frontend hit-tests buttons against its last mouse-move, ignoring button x/y.
      if (moveQueued) moveQueued = e;
      const p = pos(e);
      emit('mouse-move', p).then(() => emit('mouse-button', { button: btn[e.button] || 'left', action, ...p }));
    }, true);
  }
  for (const t of ['mousemove', 'mousedown', 'mouseup', 'click', 'dblclick', 'auxclick', 'contextmenu']) {
    addEventListener(t, swallow, true);
  }
})();
