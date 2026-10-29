const test = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

const HOST = 'api-wutheringwaves-wuwatappo.kurogames-global.com';
const url = `wss://${HOST}/socket.io/?EIO=3&transport=websocket&token=t`;

// A fake Tauri bridge plus the browser globals the shim needs.
function setup({ failConnect = false } = {}) {
  const calls = [];
  const callbacks = new Map();
  let nextId = 1;
  const nativeSockets = [];
  class NativeWebSocket { constructor(...a) { this.args = a; nativeSockets.push(this); } }
  class CloseEvent extends Event { constructor(t, i = {}) { super(t); Object.assign(this, i); } }
  const win = {
    WebSocket: NativeWebSocket,
    __TAURI_INTERNALS__: {
      transformCallback(cb) { const id = nextId++; callbacks.set(id, cb); return id; },
      unregisterCallback(id) { callbacks.delete(id); },
      invoke: async (cmd, args) => {
        calls.push([cmd, args]);
        if (cmd === 'native_ws_connect' && failConnect) throw new Error('rejected');
      },
    },
  };
  win.window = win;
  vm.runInNewContext(readFileSync(`${__dirname}/../src/ws.js`, 'utf8'), {
    window: win, location: { href: 'tauri://localhost/index.html' }, URL, EventTarget, Event, MessageEvent, CloseEvent,
    DOMException, Blob, TextEncoder, btoa, atob, ArrayBuffer, Uint8Array, Promise, Array, String, Object, Map,
  });
  // Deliver a channel message like Tauri does: {index, message}
  const deliver = (index, message) => callbacks.get(1)({ index, message });
  return { win, calls, deliver, nativeSockets, WebSocket: win.WebSocket };
}
const plain = (v) => JSON.parse(JSON.stringify(v)); // vm-realm arrays are not strictly comparable
const tick = () => new Promise((r) => setTimeout(r, 0));

test('only the multiplayer socket URL is redirected; other URLs use the browser WebSocket', () => {
  const { WebSocket, nativeSockets, calls } = setup();
  for (const other of ['wss://example.com/socket.io/', `ws://${HOST}/socket.io/`, `wss://${HOST}/other`, `wss://${HOST}:444/socket.io/`, `wss://${HOST}.evil.test/socket.io/`]) {
    new WebSocket(other);
  }
  new WebSocket('wss://example.com/x', ['p']);
  assert.equal(nativeSockets.length, 6);
  assert.deepEqual(nativeSockets[5].args, ['wss://example.com/x', ['p']]);
  assert.equal(calls.length, 0);
});

test('connects natively, then open, ordered messages and close reach the engine.io-style handlers', async () => {
  const { WebSocket, calls, deliver, nativeSockets } = setup();
  const ws = new WebSocket(url);
  assert.equal(nativeSockets.length, 0);
  assert.equal(ws.readyState, 0);
  ws.binaryType = 'arraybuffer';
  const seen = [];
  ws.onopen = () => seen.push('open');
  ws.onmessage = (e) => seen.push(e.data instanceof ArrayBuffer ? `bin:${[...new Uint8Array(e.data)]}` : `txt:${e.data}`);
  ws.onclose = (e) => seen.push(`close:${e.code}:${e.wasClean}`);
  await tick();
  assert.equal(calls[0][0], 'native_ws_connect');
  assert.equal(calls[0][1].url, url);
  assert.deepEqual(plain(calls[0][1].protocols), []);
  assert.match(calls[0][1].events, /^__CHANNEL__:\d+$/);

  deliver(0, { event: 'open', protocol: '', extensions: '' });
  assert.equal(ws.readyState, 1);
  deliver(2, { event: 'message', kind: 'binary', data: 'AQID' }); // arrives early, must wait for index 1
  deliver(1, { event: 'message', kind: 'text', data: '0{"sid":"x"}' });
  deliver(3, { event: 'close', code: 1000, reason: '', clean: true });
  assert.deepEqual(seen, ['open', 'txt:0{"sid":"x"}', 'bin:1,2,3', 'close:1000:true']);
  assert.equal(ws.readyState, 3);
});

test('send keeps order, encodes binary and refuses while connecting', async () => {
  const { WebSocket, calls, deliver } = setup();
  const ws = new WebSocket(url);
  assert.throws(() => ws.send('early'), { name: 'InvalidStateError' });
  await tick();
  deliver(0, { event: 'open', protocol: '', extensions: '' });
  ws.send('42["a"]');
  ws.send(new Uint8Array([1, 2, 3]).buffer);
  ws.send(new Blob([new Uint8Array([4, 5])]));
  await tick(); await tick(); await tick();
  const sends = calls.filter(([c]) => c === 'native_ws_send').map(([, a]) => [a.kind, a.data]);
  assert.deepEqual(sends, [['text', '42["a"]'], ['binary', 'AQID'], ['binary', 'BAU=']]);
});

test('close validates its arguments and calls the native close once', async () => {
  const { WebSocket, calls, deliver } = setup();
  const ws = new WebSocket(url);
  await tick();
  deliver(0, { event: 'open', protocol: '', extensions: '' });
  assert.throws(() => ws.close(1001), { name: 'InvalidAccessError' });
  assert.throws(() => ws.close(1000, 'x'.repeat(124)), { name: 'SyntaxError' });
  ws.close(1000, 'bye');
  ws.close();
  assert.equal(ws.readyState, 2);
  await tick(); await tick();
  assert.deepEqual(plain(calls.filter(([c]) => c === 'native_ws_close')), [['native_ws_close', { id: calls[0][1].id, code: 1000, reason: 'bye' }]]);
  deliver(1, { event: 'close', code: 1000, reason: 'bye', clean: true });
  assert.equal(ws.readyState, 3);
});

test('a failed native connect is reported as error then close, like a refused browser socket', async () => {
  const { WebSocket } = setup({ failConnect: true });
  const ws = new WebSocket(url);
  const seen = [];
  ws.onerror = () => seen.push('error');
  ws.onclose = (e) => seen.push(`close:${e.code}:${e.wasClean}`);
  await tick(); await tick();
  assert.deepEqual(seen, ['error', 'close:1006:false']);
  assert.equal(ws.readyState, 3);
});

test('native error events surface as a WebSocket error', async () => {
  const { WebSocket, deliver } = setup();
  const ws = new WebSocket(url);
  const seen = [];
  ws.addEventListener('error', () => seen.push('error'));
  ws.onclose = (e) => seen.push(`close:${e.code}`);
  await tick();
  deliver(0, { event: 'error', message: 'WebSocket handshake rejected (HTTP 403)' });
  deliver(1, { event: 'close', code: 1006, reason: '', clean: false });
  assert.deepEqual(seen, ['error', 'close:1006']);
});
