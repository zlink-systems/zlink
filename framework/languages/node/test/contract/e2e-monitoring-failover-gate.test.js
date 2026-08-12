const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('MON-A4A and MON-A4B keep replacement and crash failover as separate scenarios', () => {
  const runner = read('e2e/RuntimeMonitoring/run_e2e.sh');
  const client = read('e2e/RuntimeMonitoring/Client/main.ts');
  const replacementScenario = read('e2e/RuntimeMonitoring/Client/Scenarios/mon-a4a-normal-replacement-scenario.ts');
  const crashScenario = read('e2e/RuntimeMonitoring/Client/Scenarios/mon-a4b-crash-recovery-scenario.ts');
  const support = read('e2e/RuntimeMonitoring/Client/Support/mon-a4-availability-transition-support.ts');
  const endpoints = read('e2e/RuntimeMonitoring/Server/Service/Endpoints/service-endpoints.ts');
  const publicStatus = read('e2e/RuntimeMonitoring/Server/Service/Support/public-status-observer.ts');

  assert.match(runner, /svc-b-replacement\.config\.json/);
  assert.match(runner, /--rid svc-b[\s\S]+--channel-endpoint "\$CHANNEL_B_REPLACEMENT_ENDPOINT"/);
  assert.match(runner, /--replacement-service-url "\$SVC_B_REPLACEMENT_URL"/);
  assert.match(runner, /--replacement-service-config "\$CONFIG_DIR\/svc-b-replacement\.config\.json"/);
  assert.match(client, /await previous\?\.stop\(\)/);
  assert.match(client, /'MON-A4A': \(\) => replaceServiceBProcess\(\(\) => runMonA4A\(options\)\)/);
  assert.match(client, /'MON-A4B': async \(\) => \{ serviceBProcess = await runMonA4B\(options\); \}/);
  assert.match(runner, /source "\$NODE_ROOT\/e2e\/runner-common\.sh"/);
  assert.match(runner, /printf -v "\$variable_name" '%s' "\$\(allocate_port\)"/);
  assert.doesNotMatch(runner, /RESERVED_PORTS/);
  assert.match(replacementScenario, /runMonA4A/);
  assert.match(crashScenario, /runMonA4B/);
  assert.match(support, /startReplacementService\(options/);
  assert.match(support, /waitForRouteStatus\(/);
  assert.match(support, /options\.replacementServiceChannelEndpoint/);
  assert.match(support, /options\.serviceBChannelEndpoint/);
  assert.match(support, /after\.sequence/);
  assert.match(support, /postJson<object>\(currentUrl, '\/crash'/);
  assert.match(support, /!status\.peers\.some\(\(peer\) => peer\.nodeRid === 'svc-b'\)/);
  assert.match(support, /\/admin\/exclude/);
  assert.match(support, /readyTargetCount === 0/);
  assert.match(support, /\/admin\/include/);
  assert.match(support, /readyTargetCount === 1/);
  assert.match(endpoints, /path: '\/crash'[\s\S]*process\.kill\(process\.pid, 'SIGKILL'\)/);
  assert.doesNotMatch(support, /service drain evidence/);
  assert.match(publicStatus, /readyPeerCount/);
  assert.match(publicStatus, /readyTargetCount/);
});

test('MON-D1B keeps the registered repeated crash recovery implementation', () => {
  const client = read('e2e/RuntimeMonitoring/Client/main.ts');
  const repeatedRestartScenario = read('e2e/RuntimeMonitoring/Client/Scenarios/mon-d1b-repeated-restart-scenario.ts');

  assert.match(client, /'MON-D1B': async \(\) => \{ serviceBProcess = await runMonD1B\(options\); \}/);
  assert.match(repeatedRestartScenario, /export async function runMonD1B/);
  assert.match(repeatedRestartScenario, /for \(let cycle = 1; cycle <= 3; cycle \+= 1\)/);
  assert.match(repeatedRestartScenario, /BigInt\(restored\.sequence\) > BigInt\(before\.sequence\)/);
});

function read(relativePath) {
  return fs.readFileSync(path.join(root, relativePath), 'utf8');
}
