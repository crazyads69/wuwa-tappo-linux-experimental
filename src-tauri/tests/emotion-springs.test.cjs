const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

const bundle = readFileSync(`${__dirname}/../../dist/assets/index-CHhF-M4P.js`, 'utf8');
const start = bundle.indexOf('__name:"EmotionMenu"');
const springStart = bundle.indexOf('const S=new Map,P=Of({});', start);
const mountStart = bundle.indexOf('return Ds(', springStart);
const cleanupStart = bundle.indexOf('),vi(', mountStart);
const renderStart = bundle.indexOf('}),(X,O)=>', cleanupStart);
assert.ok(start >= 0 && springStart > start && mountStart > springStart && cleanupStart > mountStart && renderStart > cleanupStart,
  'recovered EmotionMenu boundaries changed');
const springSource = bundle.slice(springStart, mountStart);
const openSource = bundle.slice(bundle.indexOf('async function m(){', start), bundle.indexOf('Ne(()=>c.visible', start));
const timerCleanupSource = bundle.slice(bundle.indexOf('function r(){', start), bundle.indexOf('function h(){', start));
const mountSource = bundle.slice(mountStart + 'return Ds('.length, cleanupStart);
const cleanupSource = bundle.slice(cleanupStart + '),vi('.length, renderStart + 1);

function harness({ visible = true, reduced = false } = {}) {
  let now = 0, nextId = 1, writes = 0, measures = 0;
  const frames = new Map(), timers = new Map(), emitted = [];
  const f = { value: visible }, y = { value: false };
  const context = vm.createContext({
    f, y, Cv: 720, Sv: 720,
    performance: { now: () => now },
    window: { matchMedia: () => ({ matches: reduced }), addEventListener() {}, removeEventListener() {} },
    Of: (value) => new Proxy(value, { set(target, key, data) { writes += 1; target[key] = data; return true; } }),
    Oe: async () => {}, h: () => { measures += 1; }, u: (...args) => emitted.push(args),
    requestAnimationFrame: (fn) => { const id = nextId++; frames.set(id, fn); return id; },
    cancelAnimationFrame: (id) => frames.delete(id),
    setTimeout: (fn, delay) => { const id = nextId++; timers.set(id, { fn, at: now + delay }); return id; },
    clearTimeout: (id) => timers.delete(id),
  });
  const api = vm.runInContext(`let s=0,o=0;${timerCleanupSource}${springSource}${openSource}
    ({ press: U, release: V, pointerDown: G, pointerUp: ae, keyDown: de, keyUp: ke,
       wake: wakeSpringFrame, open: m, close: A, mount: ${mountSource}, unmount: ${cleanupSource},
       state: (id) => S.get(id), stateCount: () => S.size })`, context);
  function tick(ms = 1000 / 60) {
    now += ms;
    for (;;) {
      const due = [...timers].filter(([, timer]) => timer.at <= now).sort((a, b) => a[1].at - b[1].at)[0];
      if (!due) break;
      timers.delete(due[0]);
      due[1].fn();
    }
    const ready = [...frames];
    frames.clear();
    for (const [, fn] of ready) fn(now);
  }
  function settle(limit = 600) {
    for (let i = 0; frames.size || timers.size; i++) {
      assert.ok(i < limit, 'animation/timers failed to settle');
      tick();
    }
  }
  return { api, f, y, tick, settle, emitted,
    get writes() { return writes; }, get measures() { return measures; },
    get frames() { return frames.size; }, get timers() { return timers.size; } };
}

function assertRest(state, x = 1, y = 1, r = 0) {
  assert.deepEqual([state.x, state.y, state.r, state.vx, state.vy, state.vr], [x, y, r, 0, 0, 0]);
}

test('mount and empty menu do not start a permanent RAF loop', () => {
  for (const visible of [false, true]) {
    const h = harness({ visible });
    h.api.mount();
    h.api.wake();
    assert.equal(h.frames, 0);
    h.tick(60_000);
    assert.equal(h.writes, 0);
  }
});

test('held pointer spring settles and stops reactive writes', () => {
  const h = harness();
  let captures = 0;
  h.api.pointerDown(7, { pointerId: 1, currentTarget: { setPointerCapture() { captures += 1; } } });
  assert.equal(captures, 1);
  assert.equal(h.frames, 1);
  h.settle();
  assertRest(h.api.state(7), .86, .72, -.025);
  assert.equal(h.api.state(7).held, true);
  assert.ok(h.writes > 1, 'press should animate rather than snap immediately');
  const writes = h.writes;
  h.api.wake();
  h.tick(60_000);
  assert.equal(h.frames, 0);
  assert.equal(h.writes, writes);
});

test('release preserves delayed bounce and return, then rests exactly', () => {
  const h = harness();
  h.api.press(7);
  h.tick(10);
  h.api.pointerUp(7);
  h.tick(50);
  assert.equal(h.api.state(7).targetX, .86, 'short press must retain its minimum hold time');
  h.tick(20);
  assert.equal(h.api.state(7).targetX, 1.48);
  assert.equal(h.api.state(7).targetY, 1.2);
  let expanded = false;
  for (let i = 0; i < 10; i++) { h.tick(); expanded ||= h.api.state(7).x > 1; }
  assert.ok(expanded, 'release bounce must remain visible');
  h.settle();
  assertRest(h.api.state(7));
  assert.equal(h.api.state(7).held, false);
  const writes = h.writes;
  h.tick(60_000);
  assert.equal(h.frames, 0);
  assert.equal(h.writes, writes);
});

test('keyboard press and release use the same spring transitions', () => {
  const h = harness();
  let prevented = 0;
  const event = { key: ' ', preventDefault() { prevented += 1; } };
  h.api.keyDown(8, event);
  h.settle();
  assertRest(h.api.state(8), .86, .72, -.025);
  h.api.keyUp(8, event);
  h.settle();
  assertRest(h.api.state(8));
  assert.equal(prevented, 2);
});

test('hidden and reduced-motion spring state never schedules animation frames', () => {
  const hidden = harness({ visible: false });
  hidden.api.press(7);
  hidden.api.release(7);
  hidden.settle();
  assert.equal(hidden.frames, 0);
  assert.equal(hidden.writes, 0);
  const reduced = harness({ reduced: true });
  reduced.api.mount();
  reduced.api.press(7);
  reduced.api.release(7);
  reduced.api.wake();
  assert.equal(reduced.frames, 0);
  assert.equal(reduced.writes, 0);
});

test('reopening resumes unfinished springs and preserves fan transitions', async () => {
  const h = harness();
  h.api.press(7);
  h.tick();
  h.f.value = false;
  h.tick();
  assert.equal(h.frames, 0);
  const hiddenWrites = h.writes;
  h.api.release(7);
  h.settle();
  assert.equal(h.writes, hiddenWrites);
  await h.api.open();
  assert.equal(h.f.value, true);
  assert.equal(h.y.value, false);
  assert.ok(h.frames > 0);
  h.tick(); h.tick();
  assert.equal(h.y.value, true);
  h.settle();
  assertRest(h.api.state(7));
  assert.equal(h.measures, 1);
  assert.equal(h.emitted.at(-1)[0], 'settled');
  h.api.close();
  assert.equal(h.y.value, false);
  assert.equal(h.f.value, true);
  h.tick(700);
  assert.equal(h.f.value, true, 'closing fan remains mounted for its CSS transition');
  h.tick(20);
  assert.equal(h.f.value, false);
  assert.equal(h.frames, 0);
});

test('unmount cancels outstanding frames and spring timers', () => {
  const h = harness();
  h.api.press(7);
  h.api.release(7);
  assert.ok(h.frames && h.timers);
  h.api.unmount();
  assert.equal(h.frames, 0);
  assert.equal(h.timers, 0);
  assert.equal(h.api.stateCount(), 0);
});
