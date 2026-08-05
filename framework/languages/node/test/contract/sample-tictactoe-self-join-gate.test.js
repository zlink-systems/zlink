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
  assert.match(scenario, /client1\.expectNone<PlayerJoinedNotify>\(PacketNames\.playerJoinedNotify\)\.within\(250\)\.run\(signal\)/);
  assert.match(scenario, /client2\.expectNone<PlayerJoinedNotify>\(PacketNames\.playerJoinedNotify\)\.within\(250\)\.run\(signal\)/);
  assert.doesNotMatch(scenario, /watchForUnexpectedMessage|assertAbsent/);
});
