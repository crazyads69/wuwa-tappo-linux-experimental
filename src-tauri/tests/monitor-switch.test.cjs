const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const vm = require('node:vm');

const source = readFileSync(`${__dirname}/../../dist/assets/switchDisplayMonitor-2jwec8t8.js`, 'utf8');
const boundary = source.match(/async function Ht\(\)\{.*?\}(?=async function Yt)/)?.[0];
assert.ok(boundary, 'recovered monitor-switch entry point changed');
const entry = (invoke, work) => vm.runInNewContext(`let y=null;${boundary};Ht`, { f: invoke, Nt: work, console });

test('simultaneous focused-window DOM and main native F8 run one switch handshake', async () => {
  let active = null, next = 0, runs = 0, complete;
  const releases = [];
  const invoke = async (cmd, args) => {
    if (cmd === 'begin_main_monitor_switch') {
      if (active !== null) return null;
      active = ++next;
      return active;
    }
    assert.equal(cmd, 'end_main_monitor_switch');
    releases.push(args.token);
    assert.equal(args.token, active);
    active = null;
  };
  const work = () => { runs += 1; return new Promise(resolve => { complete = resolve; }); };
  const mainNative = entry(invoke, work), focusedPanelDOM = entry(invoke, work);
  const first = mainNative(), second = focusedPanelDOM();
  await new Promise(setImmediate);
  assert.equal(runs, 1);
  complete(true);
  assert.deepEqual(await Promise.all([first, second]), [true, false]);
  assert.deepEqual(releases, [1]);
  assert.equal(active, null);
});

test('failed switch releases its native lease in finally', async () => {
  const calls = [];
  const invoke = async (cmd, args) => {
    calls.push([cmd, args?.token]);
    return cmd === 'begin_main_monitor_switch' ? 42 : undefined;
  };
  const run = entry(invoke, async () => { throw new Error('output disconnected'); });
  await assert.rejects(run(), /output disconnected/);
  assert.deepEqual(calls, [['begin_main_monitor_switch', undefined], ['end_main_monitor_switch', 42]]);
});
