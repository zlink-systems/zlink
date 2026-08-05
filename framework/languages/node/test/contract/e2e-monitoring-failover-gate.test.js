const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('MON-A4 separates replacement, crash failover, and transport weight exclusion', () => {
  const runner = read('e2e/RuntimeMonitoring/run_e2e.sh');
  const client = read('e2e/RuntimeMonitoring/Client/main.ts');
  const scenario = read('e2e/RuntimeMonitoring/Client/Scenarios/mon-a4-availability-transition-scenario.ts');
  const endpoints = read('e2e/RuntimeMonitoring/Server/Service/Endpoints/service-endpoints.ts');
  const publicStatus = read('e2e/RuntimeMonitoring/Server/Service/Support/public-status-observer.ts');

  assert.match(runner, /svc-b-replacement\.config\.json/);
  assert.match(runner, /--rid svc-b[\s\S]+--channel-endpoint "\$CHANNEL_B_REPLACEMENT_ENDPOINT"/);
  assert.match(runner, /--replacement-service-url "\$SVC_B_REPLACEMENT_URL"/);
  assert.match(runner, /--replacement-service-config "\$CONFIG_DIR\/svc-b-replacement\.config\.json"/);
  assert.match(client, /replaceServiceBProcess\(\(\) => runMonA4\(options\)\)/);
  assert.match(client, /await previous\?\.stop\(\)/);
  assert.match(client, /'MON-A4A': \(\) => replaceServiceBProcess\(\(\) => runMonA4A\(options\)\)/);
  assert.match(client, /'MON-A4B': async \(\) => \{ serviceBProcess = await runMonA4B\(options\); \}/);
  assert.match(runner, /RESERVED_PORTS/);
  assert.match(scenario, /startReplacementService\(options/);
  assert.match(scenario, /waitForRouteStatus\(/);
  assert.match(scenario, /options\.replacementServiceChannelEndpoint/);
  assert.match(scenario, /options\.serviceBChannelEndpoint/);
  assert.match(scenario, /after\.sequence/);
  assert.match(scenario, /postJson<object>\(currentUrl, '\/crash'/);
  assert.match(scenario, /!status\.peers\.some\(\(peer\) => peer\.nodeRid === 'svc-b'\)/);
  assert.match(scenario, /\/admin\/exclude/);
  assert.match(scenario, /readyTargetCount === 0/);
  assert.match(scenario, /\/admin\/include/);
  assert.match(scenario, /readyTargetCount === 1/);
  assert.match(endpoints, /path: '\/crash'[\s\S]*process\.kill\(process\.pid, 'SIGKILL'\)/);
  assert.doesNotMatch(scenario, /service drain evidence/);
  assert.match(publicStatus, /readyPeerCount/);
  assert.match(publicStatus, /readyTargetCount/);
});

function read(relativePath) {
  return fs.readFileSync(path.join(root, relativePath), 'utf8');
}
