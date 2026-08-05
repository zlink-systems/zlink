import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');
const required = new Map([
  ['LOCAL_READINESS_TIMEOUT_SECONDS', '3'],
  ['LOCAL_READINESS_POLL_SECONDS', '0.1'],
  ['LOCAL_READINESS_ATTEMPTS', '30'],
  ['ROUTE_SETTLE_TIMEOUT_SECONDS', '5'],
  ['SCENARIO_SETTLE_TIMEOUT_SECONDS', '3'],
  ['HTTP_PROBE_TIMEOUT_SECONDS', '3']
]);

test('every Node e2e runner declares the common local wait policy', () => {
  const e2eRoot = path.join(root, 'e2e');
  const runners = fs.readdirSync(e2eRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .map((entry) => path.join(e2eRoot, entry.name, 'run_e2e.sh'))
    .filter((file) => fs.existsSync(file));

  assert.deepEqual(
    runners.map((runner) => path.basename(path.dirname(runner))).sort(),
    [
      'AutomaticTurnDispatch',
      'ChannelEgressRouting',
      'InstanceSpot',
      'ObservabilityOps',
      'PubSub',
      'RegistrationCodec',
      'RegistryMessaging',
      'ResilienceLifecycle',
      'RuntimeMonitoring',
      'SpotActorTransfer',
      'SpotService',
      'SubmitAdmission',
      'ToActorMessaging',
      'DiscoveryRegistryHa'
    ].sort()
  );
  for (const runner of runners) {
    const source = fs.readFileSync(runner, 'utf8');
    for (const [name, value] of required) {
      assert.match(source, new RegExp(`^${name}=${value.replace('.', '\\.')}$`, 'm'), `${runner} ${name}`);
    }
  }
});

test('previously unbounded health loops use the common readiness budget', () => {
  for (const relative of ['SpotActorTransfer/run_e2e.sh', 'ToActorMessaging/run_e2e.sh']) {
    const source = fs.readFileSync(path.join(root, 'e2e', relative), 'utf8');
    const health = source.match(/wait_health\(\) \{[\s\S]*?^\}/m)?.[0];
    assert.ok(health, `${relative} wait_health`);
    assert.match(health, /seq 1 "\$\{LOCAL_READINESS_ATTEMPTS\}"/);
    assert.match(health, /curl --max-time "\$\{HTTP_PROBE_TIMEOUT_SECONDS\}"/);
    assert.match(health, /sleep "\$\{LOCAL_READINESS_POLL_SECONDS\}"/);
    assert.doesNotMatch(health, /seq 1 (120|160)|sleep 0\.25/);
  }

  const spotService = fs.readFileSync(path.join(root, 'e2e/SpotService/run_e2e.sh'), 'utf8');
  assert.doesNotMatch(spotService, /^LOCAL_READINESS_ATTEMPTS=600$/m);
});

test('route and scenario settle budgets are wired into wait paths', () => {
  for (const relative of ['SpotActorTransfer/run_e2e.sh', 'ToActorMessaging/run_e2e.sh']) {
    const source = fs.readFileSync(path.join(root, 'e2e', relative), 'utf8');
    assert.match(source, /--timeout-ms "\$\(\(ROUTE_SETTLE_TIMEOUT_SECONDS \* 1000\)\)"/);
  }

  const spotService = fs.readFileSync(path.join(root, 'e2e/SpotService/run_e2e.sh'), 'utf8');
  assert.match(spotService, /seq 1 "\$\(\(ROUTE_SETTLE_TIMEOUT_SECONDS \* 10\)\)"/);

  const common = fs.readFileSync(path.join(root, 'e2e/runner-common.sh'), 'utf8');
  assert.match(common, /SCENARIO_SETTLE_TIMEOUT_SECONDS \* 10/);
});
