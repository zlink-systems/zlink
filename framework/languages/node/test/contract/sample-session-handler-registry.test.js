const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const workspaceRoot = path.resolve(__dirname, '..', '..');
const sessionFiles = [
  'samples/Bingo.Ts/Server/Session/Sessions/bingo-session.ts',
  'samples/SupportChat.Ts/Server/Session/Sessions/supportchat-session.ts',
  'samples/DeliveryDispatch.Ts/Server/Session/customer-session.ts',
  'samples/DeliveryDispatch.Ts/Server/CourierSession/courier-session.ts',
  'samples/GameQuest.Ts/Server/GameApi/game-api-session.ts',
  'samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Sessions/play-session.ts'
];

test('sample sessions delegate packet dispatch to the framework handler registry', () => {
  const failures = [];
  for (const relativePath of sessionFiles) {
    const source = fs.readFileSync(path.join(workspaceRoot, relativePath), 'utf8');
    const dispatchBody = source.match(/async onDispatch[\s\S]*?(?=\n  async onDisconnected|\n}\n)/)?.[0] ?? '';
    if (!/context\.handlers\.tryHandle\(dispatch, payload\)/.test(dispatchBody)) {
      failures.push(`${relativePath}: missing handler registry delegation`);
    }
    if (/\b(?:if|switch)\s*\([^)]*dispatch\.packetName/.test(dispatchBody)) {
      failures.push(`${relativePath}: packet-name branch remains in session`);
    }
  }
  assert.deepEqual(failures, []);
});
