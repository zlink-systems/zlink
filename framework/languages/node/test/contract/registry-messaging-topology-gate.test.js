const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

function source(relativePath) {
  return fs.readFileSync(path.join(root, relativePath), 'utf8');
}

test('RM-A1 obtains peer rows and connection evidence from application roles', () => {
  const runner = source('e2e/RegistryMessaging/run_e2e.sh');
  const consumerHost = source('e2e/RegistryMessaging/Server/Consumer/consumer-host-factory.ts');
  const endpoints = source('e2e/RegistryMessaging/Server/Consumer/Endpoints/consumer-endpoints.ts');
  const scenario = source('e2e/RegistryMessaging/Client/Scenarios/rm-a1-discovery-request-scenario.ts');

  assert.doesNotMatch(runner, /start_configured_server location-probe/);
  assert.match(consumerHost, /ZLINK_LOCATION_RUNTIME_QUERY/);
  assert.match(endpoints, /locationQuery\.listMeshNodeDescriptors/);
  assert.doesNotMatch(endpoints, /state:\s*ZLinkLocationTopologyState\.Ready/);
  assert.match(scenario, /api-a.*api-b/s);
  assert.match(scenario, /providerEvidence[\s\S]*providerAUrl/);
  assert.match(scenario, /providerEvidence[\s\S]*providerBUrl/);
});
