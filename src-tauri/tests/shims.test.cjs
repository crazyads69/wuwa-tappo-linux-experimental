const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

test('pointer button updates position first and does not stall later moves', async () => {
  const handlers = {}, frames = [], events = [];
  const window = {
    devicePixelRatio: 2,
    __wwRegions: [{ left: 10, top: 20, width: 100, height: 100, memberId: 7 }],
    __TAURI_INTERNALS__: {
      metadata: { currentWindow: { label: 'main' } },
      transformCallback() { return 1; },
      async invoke(cmd, args) {
        if (cmd.endsWith('inner_position')) return { x: 0, y: 0 };
        if (cmd.endsWith('scale_factor')) return 2;
        if (cmd === 'plugin:event|emit') events.push(args);
      },
    },
  };
  vm.runInNewContext(readFileSync(`${__dirname}/../src/shim.js`, 'utf8'), {
    window, setInterval() {},
    addEventListener: (name, fn) => { handlers[name] = fn; },
    requestAnimationFrame: (fn) => frames.push(fn),
  });
  const pointer = (x, y) => ({ isTrusted: true, clientX: x, clientY: y, button: 0, stopImmediatePropagation() {}, preventDefault() {} });
  handlers.pointerdown(pointer(30, 40));
  await new Promise(setImmediate);
  assert.deepEqual(JSON.parse(JSON.stringify(events)), [
    { event: 'mouse-move', payload: { x: 60, y: 80, memberId: 7 } },
    { event: 'mouse-button', payload: { button: 'left', action: 'press', x: 60, y: 80, memberId: 7 } },
  ]);
  handlers.pointermove(pointer(35, 45));
  handlers.pointerup(pointer(40, 50));
  await new Promise(setImmediate);
  frames.shift()();
  assert.equal(events.at(-1).payload.x, 80);
  handlers.pointermove(pointer(50, 60));
  assert.equal(frames.length, 1);
  frames.shift()();
  assert.equal(events.at(-1).payload.x, 100);
  const before = events.length;
  const synthetic = { ...pointer(50, 60), isTrusted: false,
    stopImmediatePropagation() { assert.fail('synthetic click must reach the frontend'); },
    preventDefault() { assert.fail('synthetic click must retain its default action'); },
  };
  handlers.pointerdown(synthetic);
  handlers.pointerup(synthetic);
  handlers.click(synthetic);
  await new Promise(setImmediate);
  assert.equal(events.length, before);
});

test('monitor-origin event updates an immediate click and defeats an older in-flight refresh', async () => {
  const handlers = {}, events = [], callbacks = new Map();
  let finishOldRefresh, listener;
  const window = { devicePixelRatio: 2, __TAURI_INTERNALS__: {
    metadata: { currentWindow: { label: 'main' } },
    transformCallback(fn) { callbacks.set(1, fn); return 1; },
    async invoke(cmd, args) {
      if (cmd === 'plugin:event|listen') { listener = args; return 10; }
      if (cmd.endsWith('inner_position')) return new Promise(resolve => { finishOldRefresh = resolve; });
      if (cmd.endsWith('scale_factor')) return 2;
      if (cmd === 'plugin:event|emit') events.push(args);
    },
  } };
  vm.runInNewContext(readFileSync(`${__dirname}/../src/shim.js`, 'utf8'), {
    window, setInterval() {}, addEventListener: (name, fn) => { handlers[name] = fn; },
    requestAnimationFrame() { assert.fail('this regression requires no animation frame or resize'); },
  });
  assert.equal(listener.event, 'cat-main-monitor-origin-changed');
  callbacks.get(listener.handler)({ payload: { origin: { x: 1920, y: 0 }, monitor: { scaleFactor: 2 }, layerShell: true } });
  const click = { isTrusted: true, clientX: 30, clientY: 40, button: 0, stopImmediatePropagation() {}, preventDefault() {} };
  handlers.pointerdown(click);
  await new Promise(setImmediate);
  assert.equal(events.at(-1).payload.x, 1980);
  assert.equal(events.at(-1).payload.y, 80);
  finishOldRefresh({ x: 0, y: 0 });
  await new Promise(setImmediate);
  handlers.pointerup(click);
  await new Promise(setImmediate);
  assert.equal(events.at(-1).payload.x, 1980, 'stale refresh must not undo the monitor event');
});

test('cookie shim persists across windows and honours expiry', () => {
  const storage = new Map();
  const makeDocument = () => {
    const document = {};
    vm.runInNewContext(readFileSync(`${__dirname}/../src/cookies.js`, 'utf8'), {
      document, location: { protocol: 'tauri:' },
      localStorage: { getItem: (k) => storage.get(k) ?? null, setItem: (k, v) => storage.set(k, v) },
    });
    return document;
  };
  const first = makeDocument();
  first.cookie = 'test_session=a=b; path=/; max-age=60';
  const second = makeDocument();
  assert.equal(second.cookie, 'test_session=a=b');
  second.cookie = 'test_session=; max-age=0';
  assert.equal(first.cookie, '');
});

test('Wayland monitor bridge exposes the selected output to the F8 frontend handshake', async () => {
  const monitor = { position: { x: -1920, y: 200 }, size: { width: 1920, height: 1080 }, scaleFactor: 2 };
  const origin = monitor.position;
  const window = { devicePixelRatio: 2, __TAURI_INTERNALS__: {
    metadata: { currentWindow: { label: 'main' } },
    async invoke(cmd, args) {
      if (cmd === 'get_main_monitor_snapshot') return { monitor, origin, layerShell: true };
      if (cmd === 'monitor_from_physical_point') { assert.equal(args.x, -960); return monitor; }
      if (cmd === 'plugin:window|inner_position' && args.label === 'panel') return { x: 55, y: 66 };
      assert.fail(`unexpected command ${cmd}`);
    },
  } };
  Object.defineProperty(window.__TAURI_INTERNALS__, 'invoke', { writable: false, configurable: false });
  vm.runInNewContext(readFileSync(`${__dirname}/../src/platform.js`, 'utf8'), {
    window, setInterval() {}, addEventListener() {}, requestAnimationFrame() {},
  });
  const bundle = readFileSync(`${__dirname}/../../dist/assets/index-ULLd3gpz.js`, 'utf8');
  const entry = bundle.match(/async function invoke\$1\(e,t=\{\},n\)\{[^}]+\}/);
  assert.ok(entry, 'recovered invoke boundary changed');
  const invoke = vm.runInNewContext(`${entry[0]};invoke$1`, { window });
  assert.equal(await invoke('plugin:window|current_monitor', { label: 'main' }), monitor);
  assert.equal(await invoke('plugin:window|outer_position', { label: 'main' }), origin);
  assert.equal(await invoke('plugin:window|inner_position', { label: 'main' }), origin);
  assert.equal(await invoke('plugin:window|scale_factor', { label: 'main' }), 2);
  assert.equal(await invoke('plugin:window|monitor_from_point', { x: -960, y: 500 }), monitor);
  assert.deepEqual(await invoke('plugin:window|inner_position', { label: 'panel' }), { x: 55, y: 66 });
});
