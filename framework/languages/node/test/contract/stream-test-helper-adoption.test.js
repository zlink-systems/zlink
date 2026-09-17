const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const test = require('node:test');

const sampleClients = [
  'samples/Bingo.Ts/Client/bingo-client-scenario.ts',
  'samples/DeliveryDispatch.Ts/Client/deliverydispatch-client-scenario.ts',
  'samples/GameQuest.Ts/Client/gamequest-client-scenario.ts',
  'samples/ShoppingMall.Ts/Client/shoppingmall-client-scenario.ts',
  'samples/SupportChat.Ts/Client/supportchat-client-scenario.ts',
  'samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts'
];

test('stream-connector sample scenarios use the connector test helper surface', () => {
  const samples = sampleClients.map(read).join('\n');
  assert.doesNotMatch(samples, /function (?:ensure|expectFailure|expectNoPush|expectRequestFailure|watchForUnexpectedMessage)\b/);
  assert.doesNotMatch(samples, /\.waitFor\([^\n]+\)\.timeout\(250\)/);
  assert.match(samples, /zlinkStreamAssert\.ensure\(/);
  assert.match(samples, /zlinkStreamAssert\.expectFailure\(/);
  assert.match(samples, /\.expectNone(?:<[^>]+>)?\([^\n]+\)\.within\(250\)\.run\(signal\)/);
  assert.match(samples, /\.waitForSequence(?:<[^>]+>)?\(/);
  assert.doesNotMatch(samples, /const statusWaits = statuses\.map/);
});

function read(path) {
  return readFileSync(require.resolve(`../../${path}`), 'utf8');
}
