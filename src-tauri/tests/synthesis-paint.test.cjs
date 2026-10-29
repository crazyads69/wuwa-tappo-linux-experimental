const test = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');

// Moving the five reused, filtered flex children leaves stale shadow pixels in WebKitGTK.
// A mode switch already clears selections; remount slots then, but keep identity within a mode.
test('synthesis slots have stable keys within a mode and fresh keys across modes', () => {
  const source = fs.readFileSync(path.join(__dirname, '../../dist/assets/index-B8v13rah.js'), 'utf8');
  const expression = source.match(/ke\(Me\.value,\(y,j\)=>\(l\(\),v\("div",\{key:([^,]+),class:se\(\["synthesis-item"/)[1];
  const key = new Function('Y', 'j', `return ${expression}`);
  const upgrade = Array.from({ length: 10 }, (_, j) => key({ value: 0 }, j));
  const sameTier = Array.from({ length: 5 }, (_, j) => key({ value: 1 }, j));
  assert.equal(new Set(upgrade).size, 10);
  assert.equal(new Set(sameTier).size, 5);
  assert(sameTier.every(k => !upgrade.includes(k)), 'no moving filtered slots may survive a mode switch');
  assert.deepEqual(upgrade, Array.from({ length: 10 }, (_, j) => key({ value: 0 }, j)));
});
