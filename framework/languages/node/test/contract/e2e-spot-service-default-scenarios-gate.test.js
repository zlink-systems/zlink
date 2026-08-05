const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('SpotService all runs only scenarios defined by the common config', () => {
  const runner = fs.readFileSync(path.join(root, 'e2e/SpotService/run_e2e.sh'), 'utf8');
  const allChildren = runner.match(/for child_group in ([^;]+); do/);

  assert.ok(allChildren, 'SpotService all child list is missing.');
  assert.doesNotMatch(allChildren[1], /\bSM-Q9\b/);
  assert.match(runner, /SCENARIO" == "SM-Q9"/);
});
