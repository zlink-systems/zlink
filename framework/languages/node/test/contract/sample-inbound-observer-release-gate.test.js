const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('Bingo and TicTacToe require inbound observations for every client role', () => {
  const bingo = fs.readFileSync(path.join(root, 'samples/Bingo.Ts/Client/main.ts'), 'utf8');
  const ticTacToe = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts'
  ), 'utf8');

  for (const role of ['player-1', 'player-2', 'observer']) {
    assert.match(bingo, new RegExp(`assertInboundObserved\\(observedClients, '${role}'\\)`));
  }
  for (const role of ['host', 'guest', 'observer']) {
    assert.match(ticTacToe, new RegExp(`assertInboundObserved\\(observedClients, '${role}'\\)`));
  }
  assert.match(bingo, /observation\.name\.length > 0/);
  assert.match(ticTacToe, /observation\.payloadLength >= 0/);
});
