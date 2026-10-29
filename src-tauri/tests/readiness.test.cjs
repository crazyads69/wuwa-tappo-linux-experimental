const {test} = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const html = fs.readFileSync(path.join(__dirname, '../../dist/index.html'), 'utf8');
const gate = [...html.matchAll(/<script>([\s\S]*?)<\/script>/g)][0][1];
function harness({route = '#/panel', content = false, fonts = 'loading', rejectFonts = false} = {}) {
  let now = 0;
  const timers = [], attrs = new Map(), calls = [];
  const state = {content, fonts};
  const root = {classList: {contains: c => c === 'panel-window-root'}, querySelector: () => state.content};
  const document = {
    documentElement: {setAttribute: (k,v) => attrs.set(k,v), removeAttribute: k => attrs.delete(k)},
    getElementById: () => ({firstElementChild: root}), body: {offsetHeight: 760}, querySelectorAll: () => [],
    fonts: {get status() {return state.fonts}, ready: Promise.resolve(),
      load: () => rejectFonts ? Promise.reject(new Error('font unavailable')) : Promise.resolve([])},
  };
  const window = {__TAURI_INTERNALS__: {invoke: c => {calls.push(c); return Promise.resolve()}}};
  const timeout = (fn, ms) => timers.push({fn, at: now + ms});
  vm.runInNewContext(gate, {document, window, location: {hash: route}, performance: {now: () => now},
    localStorage: {getItem: () => 'en'}, navigator: {languages: ['en']}, setTimeout: timeout,
    requestAnimationFrame: fn => timeout(fn,16), getComputedStyle: () => ({}), Image: class {}});
  return {state, attrs, calls, async tick(ms) {
    const end = now + ms;
    for (let i = 0; i < 10000; i++) {
      for (let j=0;j<12;j++) await Promise.resolve();
      timers.sort((a,b) => a.at-b.at);
      if (!timers.length || timers[0].at > end) break;
      const t = timers.shift(); now = t.at; t.fn();
    }
    now = end;
    for (let i=0;i<12;i++) await Promise.resolve();
  }};
}
test('native gate precedes reveal and waits for content and fonts', async () => {
  const h = harness();
  assert.deepEqual(h.calls, ['window_gate']);
  await h.tick(100);
  assert(h.attrs.has('data-ww-gate'));
  h.state.content = true;
  await h.tick(100);
  assert(h.attrs.has('data-ww-gate'), 'loaded DOM with pending fonts remains hidden');
  h.state.fonts = 'loaded';
  await h.tick(300);
  assert(!h.attrs.has('data-ww-gate'));
  assert.deepEqual(h.calls, ['window_gate','window_ready']);
  assert.match(h.attrs.get('data-ww-ready'), /^ready@/);
});
test('failed loading cannot leave the UI hidden forever', async () => {
  const h = harness({rejectFonts:true});
  await h.tick(4100);
  assert(!h.attrs.has('data-ww-gate'));
  assert.match(h.attrs.get('data-ww-ready'), /^timeout@/);
  assert.equal(h.calls.filter(c=>c==='window_ready').length,1);
});
test('toast overlay does not block its independent readiness handshake', () => {
  const h = harness({route:'#/toast'});
  assert.equal(h.attrs.size,0);
  assert.deepEqual(h.calls,[]);
});
