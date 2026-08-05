const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const root = path.resolve(__dirname, '../..');

test('TicTacToe nextTurn remains a non-null wire string', () => {
  const messages = fs.readFileSync(path.join(root, 'samples/TicTacToe.Ts/Shared/Contracts/messages.ts'), 'utf8');
  const match = fs.readFileSync(path.join(
    root,
    'samples/TicTacToe.Ts/Server/Play/Domain/TicTacToe/tictactoe-match.ts'
  ), 'utf8');
  const client = fs.readFileSync(path.join(root, 'samples/TicTacToe.Ts/Client/tictactoe-client-scenario.ts'), 'utf8');

  assert.match(messages, /nextTurn: string;/);
  assert.doesNotMatch(messages, /nextTurn: string \| null/);
  assert.match(match, /private nextTurn: string;/);
  assert.doesNotMatch(match, /this\.nextTurn = null/);
  assert.match(client, /stateOf\(client1FinalMove\)\.nextTurn === ''/);
});
