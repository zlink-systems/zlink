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

test('session binding and cross-Object-Mesh Actor dispatch have canonical process runners', () => {
  const client = read('e2e/SpotService/Client/main.ts');
  const scenario = read('e2e/SpotService/Client/Scenarios/sm-d16-scenario.ts');
  const session = read('e2e/SpotService/Server/Session/session-host-factory.ts');
  const runner = read('e2e/SpotService/run_e2e.sh');

  assert.match(client, /'session-binding-e2e': \['SM-D4A', 'SM-D4B', 'SM-D5', 'SM-D5A'\]/);
  assert.match(client, /'cross-mesh-actor-dispatch': \['SM-D16'\]/);
  assert.match(scenario, /SpotServiceNames\.spotChannel/);
  assert.match(scenario, /SpotServiceNames\.spotOnlyMesh/);
  assert.match(scenario, /await pingActor\(client, primaryActorId/);
  assert.match(scenario, /await pingActor\(client, alternateActorId/);
  assert.match(session, /alternateObjectMesh\.objects\(\)\.client\(\)/);
  assert.match(session, /\.enableActorDispatch\(\)/);
  assert.match(runner, /playAlternateObjectRouterEndpoints "multi-node-a=\$MULTI_A_SPOT_ROUTER"/);
  assert.match(runner, /cross-mesh-actor-dispatch\)\s*\n\s*SERVER_ROLES=\(play-a session-a multi-node-a\)/);
});

test('native command 48 process fixture drops one reply and proves terminal replay', () => {
  const testSource = read('test/contract/user-spot-native-two-process.test.js');
  const fixture = read('test/contract/fixtures/user-spot-native-process.js');

  assert.match(testSource, /const dropped = proxy\.dropNextServerChunk\(\)/);
  assert.match(testSource, /await target\.command\('releaseFirstClose'\)/);
  assert.match(testSource, /target\.command\('closeExecutions'\), 1/);
  assert.match(testSource, /proxy\.connectionCount >= 2/);
  assert.match(fixture, /ZLINK_TEST_PAUSE_FIRST_CLOSE/);
  assert.match(fixture, /type: 'close-entered'/);
});
