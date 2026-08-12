const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const scenarioPath = path.resolve(
  __dirname,
  '../../samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts'
);

test('TicTacToe proves self-join notification absence across the join barrier', () => {
  const scenario = fs.readFileSync(scenarioPath, 'utf8');

  assert.doesNotMatch(scenario, /\.timeout\(25\)/);
  assert.match(
    scenario,
    /const client1SelfJoin = client1\s*\.expectNone<PlayerJoinedNotify>\(PacketNames\.playerJoinedNotify\)\s*\.within\(250\)\s*\.run\(signal\)/
  );
  assert.match(
    scenario,
    /const client2SelfJoin = client2\s*\.expectNone<PlayerJoinedNotify>\(PacketNames\.playerJoinedNotify\)\s*\.within\(250\)\s*\.run\(signal\)/
  );
  assert.match(scenario, /client1JoinedState,\s*client1SelfJoin/);
  assert.match(scenario, /client2JoinedState,\s*client1SawClient2Join,\s*client2SelfJoin/);
  assert.doesNotMatch(scenario, /watchForUnexpectedMessage|assertAbsent/);
});
