#!/usr/bin/env node
// Checks the panel-route patches in dist/assets/index-B8v13rah.js (run scripts/setup.sh first).
// Usage: node tools/check-panel-patches.cjs
const fs = require('fs'), path = require('path'), assert = require('assert');
const dist = path.join(__dirname, '..', 'dist');
const js = fs.readFileSync(path.join(dist, 'assets/index-B8v13rah.js'), 'utf8');
const css = fs.readFileSync(path.join(dist, 'assets/index-BjTCKBy6.css'), 'utf8');
const html = fs.readFileSync(path.join(dist, 'index.html'), 'utf8');

// Markers: each patch must be present exactly once.
for (const m of ['f.value=ww(m)', 'collapsed:dc.value,', '"panel-window-root--sync":sy.value', 'wwP=C;wwT=Date.now()']) {
  assert.strictEqual(js.split(m).length - 1, 1, m);
}
assert(css.includes('.panel-window-root--sync .module-content{flex:0 0 849px}'));
// collapse/expand never resizes the native window: the card narrows by the 142 px rail difference instead
assert(css.includes('.panel-window-root .module-panel--collapsed{width:calc(100% - 142px)!important}'));
assert(js.includes('dc.value=C;co(C).catch(bad)') && !js.includes('setTimeout(r,320)'), 'collapse watcher must not sequence a native resize');
assert(html.includes('data-ww-gate') && html.includes('document.fonts.status'));

// Behaviour of the optimistic-tab guard, extracted from the shipped code.
const code = js.slice(js.indexOf('function ww(m)'), js.indexOf('function wwW('));
const { ww, setPending } = new Function('let wwP=null,wwT=0;' + code + 'return {ww,setPending:v=>{wwP=v;wwT=Date.now()}};')();
assert.strictEqual(ww({ activeTab: 'bag' }).activeTab, 'bag');
setPending('atlas');
assert.strictEqual(ww({ activeTab: 'bag' }).activeTab, 'atlas', 'stale in-flight state cannot flip the tab back');
assert.strictEqual(ww({ activeTab: 'atlas' }).activeTab, 'atlas');
assert.strictEqual(ww({ activeTab: 'bag' }).activeTab, 'bag', 'guard is released once main confirms');
console.log('panel patch checks passed');
