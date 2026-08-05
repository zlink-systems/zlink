const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

function source(relativePath) {
  return fs.readFileSync(path.join(root, relativePath), 'utf8');
}

test('Config 1, 2, and 9 execute reproducible start-order axes', () => {
  const registry = source('e2e/RegistryMessaging/run_e2e.sh');
  const spot = source('e2e/SpotService/run_e2e.sh');
  const actor = source('e2e/ToActorMessaging/run_e2e.sh');
  const sweep = source('e2e/run_e2e_all.sh');
  const common = source('e2e/runner-common.sh');

  for (const runner of [registry, spot, actor]) {
    assert.match(runner, /E2E_START_ORDER/);
    assert.match(runner, /ordered_e2e_roles/);
  }
  assert.match(common, /shuffle:/);
  assert.match(sweep, /run_config_with_retry.*reverse/);
  assert.match(sweep, /run_config_with_retry.*shuffle:20260715/);
  assert.match(sweep, /RegistryMessaging/);
  assert.match(sweep, /SpotService/);
  assert.match(sweep, /ToActorMessaging/);
});
