const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

const bundle = readFileSync(`${__dirname}/../../dist/assets/index-ULLd3gpz.js`, 'utf8');

// Run the recovered auth module itself; fail explicitly if a future bundle changes its boundaries.
function section(start, end) {
  const from = bundle.indexOf(start), to = bundle.indexOf(end, from);
  assert.ok(from >= 0 && to > from, `missing auth source boundary: ${start}`);
  assert.equal(bundle.indexOf(start, from + start.length), -1, `ambiguous auth source boundary: ${start}`);
  return bundle.slice(from, to);
}

function authHarness() {
  const storage = new Map(), opened = [], errors = [], accepted = [];
  let now = 1_800_000_000_000, onUrl;
  const context = vm.createContext({
    URL, Math,
    Date: class extends Date { static now() { return now; } },
    crypto: { randomUUID: () => '12345678-1234-1234-1234-123456789abc' },
    localStorage: {
      getItem: (k) => storage.get(k) ?? null,
      setItem: (k, v) => storage.set(k, String(v)),
      removeItem: (k) => storage.delete(k),
    },
    i18n: { global: { locale: { value: 'en' } } },
    getSdkLangForLocale: (locale) => locale,
    openUrl: async (url) => { opened.push(url); },
    getCurrent: async () => null,
    onOpenUrl: async (fn) => { onUrl = fn; return () => { onUrl = null; }; },
    console: { warn() {}, error() {} },
  });
  const code = section('const PENDING_STATE_KEY=', 'const KR_SDK_SCRIPT_LOAD_TIMEOUT_MS=');
  const api = vm.runInContext(`${code};({parseOverseasAuthCallback,rememberPendingState,readPendingState,startOverseasBrowserLogin,bindOverseasAuthDeepLink})`, context);
  return {
    api, context, storage, opened, errors, accepted,
    advance: (ms) => { now += ms; },
    async bind(onSuccess = (payload) => { accepted.push(payload); }) {
      await api.bindOverseasAuthDeepLink({ onSuccess, onError: (error) => { errors.push(error); } });
    },
    async deliver(url) { onUrl([url]); await new Promise(setImmediate); },
  };
}

const callback = (state, extra = 'access_token=fixture-token&cuid=fixture-user') =>
  `wuwa-buddy://auth/callback?state=${state}&${extra}`;

test('browser login preserves its random pending state and configured scheme', async () => {
  const h = authHarness();
  const { state } = await h.api.startOverseasBrowserLogin({ lang: 'en' });
  const url = new URL(h.opened[0]);
  assert.equal(url.origin, 'https://login-wutheringwaves-wuwatappo.kurogames-global.com');
  assert.equal(url.searchParams.get('scheme'), 'wuwa-buddy');
  assert.equal(url.searchParams.get('state'), state);
  assert.equal(h.api.readPendingState(), state);
});

test('callback rejects invalid input, mismatched or missing state, and replay', async () => {
  const h = authHarness();
  await h.bind();
  for (const url of ['not a URL', callback('expected').replace('wuwa-buddy:', 'https:'),
    'wuwa-buddy://auth/callback?access_token=fixture-token', callback('expected', '')]) {
    assert.equal(h.api.parseOverseasAuthCallback(url), null);
  }
  await h.deliver(callback('no-pending-state'));
  assert.equal(h.accepted.length, 0);
  h.api.rememberPendingState('expected');
  await h.deliver(callback('wrong'));
  assert.equal(h.accepted.length, 0);
  assert.equal(h.api.readPendingState(), 'expected');
  await h.deliver(callback('expected'));
  assert.equal(h.accepted.length, 1);
  assert.equal(h.accepted[0].cuid, 'fixture-user');
  assert.equal(h.api.readPendingState(), null);
  await h.deliver(callback('expected'));
  assert.equal(h.accepted.length, 1);
  assert.equal(h.errors.length, 2);
});

test('pending browser login expires after fifteen minutes', async () => {
  const h = authHarness();
  await h.bind();
  h.api.rememberPendingState('expected');
  h.advance(900_001);
  await h.deliver(callback('expected'));
  assert.equal(h.accepted.length, 0);
  assert.equal(h.api.readPendingState(), null);
  assert.equal(h.storage.has('overseas-oauth-pending-at'), false);
});

test('validated callback reaches player-lookup boundary; code-only callbacks never do', async () => {
  const h = authHarness(), persisted = [], playersRequested = [], failures = [];
  const boundary = new Error('fixture stops before any network request');
  Object.assign(h.context, {
    useLoading: () => ({ showLoading() {}, hideLoading() {} }),
    clearPendingOverseasRoleSelect() {},
    fpersistBrowserAuthSession: (session) => { persisted.push(session); },
    fetchForeignSdkPlayers: async (cuid) => { playersRequested.push(cuid); throw boundary; },
    showApiErrorToast: (error) => { failures.push(error); },
    reportAlert() {}, logDiagnostic() {}, diagnosticErrorCode: () => 'fixture', dispatchLoginOverlayOpen() {},
  });
  const method = section('async completeForeignBrowserLogin(e){', ',async tryResumeOverseasRoleSelect()');
  // CJS VM has no import.meta; only the unexecuted post-network dynamic-import branch uses this URL.
  assert.equal(method.split('import.meta.url').length, 2);
  const service = vm.runInContext(`({${method.replace('import.meta.url', '"tauri://localhost/index.html"')}})`, h.context);
  await h.bind((payload) => service.completeForeignBrowserLogin(payload));
  h.api.rememberPendingState('expected');
  await h.deliver(callback('expected'));
  assert.deepEqual(playersRequested, ['fixture-user']);
  assert.equal(persisted[0].accessToken, 'fixture-token');
  assert.equal(failures[0], boundary);
  assert.equal(service.loading, false);

  h.api.rememberPendingState('code-only');
  await h.deliver(callback('code-only', 'code=fixture-code&cuid=fixture-user'));
  h.api.rememberPendingState('no-user');
  await h.deliver(callback('no-user', 'access_token=fixture-token'));
  assert.equal(playersRequested.length, 1);
  assert.equal(persisted.length, 1);
  assert.equal(failures.length, 3);
});
