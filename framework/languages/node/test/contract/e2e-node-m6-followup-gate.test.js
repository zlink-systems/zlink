const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');
const read = (relative) => fs.readFileSync(path.join(root, relative), 'utf8');

test('samples and E2E use the exact no-argument Actor dispatch opt-in', () => {
  for (const relative of [
    'samples/Bingo.Ts/Server/Session/bingo-session-module.ts',
    'e2e/SpotService/Server/Session/session-host-factory.ts'
  ]) {
    const source = read(relative);
    assert.match(source, /\.enableActorDispatch\(\)/);
    assert.doesNotMatch(source, /\.enableActorDispatch\([^)]/);
  }
});

test('session binding and channel-to-Actor push have canonical process runners', () => {
  const client = read('e2e/SpotService/Client/main.ts');
  const scenario = read('e2e/SpotService/Client/Scenarios/sm-d15-scenario.ts');
  const session = read('e2e/SpotService/Server/Session/session-host-factory.ts');
  const runner = read('e2e/SpotService/run_e2e.sh');

  assert.match(client, /'session-binding-e2e': \['SM-D4A', 'SM-D4B', 'SM-D5', 'SM-D5A'\]/);
  assert.match(client, /'SM-D15': \(\) => runSmD15\(options\)/);
  assert.match(scenario, /CrossRoleActorPushReq/);
  assert.match(scenario, /\/actor\/cross-role\/push/);
  assert.match(scenario, /waitFor<ActorPushNotify>/);
  assert.match(session, /\.enableActorDispatch\(\)/);
  assert.match(session, /\.registerSession\(ScenarioSessionFactory\)/);
  assert.match(runner, /\*\) SERVER_ROLES=\(play-a play-b session-a session-b gateway multi-node-a multi-node-b\)/);
});

test('native process fixture pauses one close and proves terminal cleanup', () => {
  const testSource = read('test/contract/user-spot-native-two-process.test.js');
  const fixture = read('test/contract/fixtures/user-spot-native-process.js');

  assert.match(testSource, /await target\.waitForEvent\('close-entered'\)/);
  assert.match(testSource, /await target\.command\('releaseFirstClose'\)/);
  assert.match(testSource, /target\.command\('closeExecutions'\), 1/);
  assert.match(fixture, /ZLINK_TEST_PAUSE_FIRST_CLOSE/);
  assert.match(fixture, /type: 'close-entered'/);
});
