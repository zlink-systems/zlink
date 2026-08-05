import assert from 'node:assert/strict';
import fs from 'node:fs';
import path from 'node:path';
import test from 'node:test';

const root = path.resolve(import.meta.dirname, '../..');

test('every Node e2e runner writes execution logs under log', () => {
  const e2eRoot = path.join(root, 'e2e');
  const ignore = fs.readFileSync(path.join(e2eRoot, '.gitignore'), 'utf8');
  assert.match(ignore, /^\*\/log\/\*$/m);
  assert.match(ignore, /^!\*\/log\/\.gitignore$/m);
  const configs = fs.readdirSync(e2eRoot, { withFileTypes: true })
    .filter((entry) => entry.isDirectory())
    .filter((entry) => fs.existsSync(path.join(e2eRoot, entry.name, 'run_e2e.sh')));

  assert.deepEqual(
    configs.map((config) => config.name).sort(),
    [
      'AutomaticTurnDispatch',
      'ChannelEgressRouting',
      'DiscoveryRegistryHa',
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
      'ToActorMessaging'
    ].sort()
  );
  for (const config of configs) {
    const configRoot = path.join(e2eRoot, config.name);
    const runner = fs.readFileSync(path.join(configRoot, 'run_e2e.sh'), 'utf8');
    assert.match(runner, /LOG_DIR="\$ROOT_DIR\/log\/\$RUN_ID"/, config.name);
    assert.doesNotMatch(runner, /ROOT_DIR\/logs\//, config.name);
    assert.equal(fs.existsSync(path.join(configRoot, 'log/.gitignore')), true, config.name);
  }
});
