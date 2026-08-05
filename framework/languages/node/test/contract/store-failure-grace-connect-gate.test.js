const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

function source(relativePath) {
  return fs.readFileSync(path.join(root, relativePath), 'utf8');
}

test('SF-B2 restarts a provider during outage and rejects the new connection', () => {
  const runner = source('e2e/DiscoveryRegistryHa/run_e2e.sh');
  const scenario = source('e2e/DiscoveryRegistryHa/Client/Scenarios/SfB2StoreFailureGraceScenario.ts');

  assert.match(runner, /run_sf_b2[\s\S]*stop_redis[\s\S]*kill_pid "\$API_B_PID"[\s\S]*start_provider_b/);
  assert.match(scenario, /reply\.providerRid === 'api-a'/);
  assert.match(scenario, /providerBUrl/);
  assert.match(scenario, /sf-b2/);
  assert.match(scenario, /api-b.*evidence/s);
});
