const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

function read(relativePath) {
  return fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');
}

test('GameQuest proves concurrent players use framework-selected owner nodes', () => {
  const client = read('samples/GameQuest.Ts/Client/gamequest-client-scenario.ts');
  const store = read('samples/GameQuest.Ts/Server/Shared/Store/quest-progress-store.ts');

  assert.match(client, /await Promise\.all\(\[/);
  assert.match(store, /`owner:\$\{owner\}:\$\{playerId\}`/);
  assert.match(client, /\/\^owner:mission-\[ab\]:player-alice\$\/\.test\(entry\)/);
  assert.match(client, /\/\^owner:mission-\[ab\]:player-bob\$\/\.test\(entry\)/);
  assert.doesNotMatch(client, /NodeRid|routingId|mission-a:player-alice|mission-b:player-bob/);
});
