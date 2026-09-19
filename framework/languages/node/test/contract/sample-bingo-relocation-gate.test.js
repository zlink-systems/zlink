const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const read = relativePath => fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');

test('Bingo preserves room domain state and defers relocation at the completed-round boundary', () => {
  const module = read('samples/Bingo.Ts/Server/Play/bingo-play-module.ts');
  const room = read('samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts');
  const adapter = read('samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-relocation-adapter.ts');

  assert.match(module, /ZLinkSpotRelocationCoordinationMode\.ApplicationSignaled/);
  assert.match(module, /\.preserveStateWith\(BingoRoomRelocationAdapter\)/);
  assert.doesNotMatch(module, /\.disableRelocation\(\)/);
  assert.match(room, /await this\.publishReward\(state\);[\s\S]*?await this\.leaveFinishedActors\(\);[\s\S]*?this\.context\.relocationReady\(\)\.defer\(\)/);
  assert.match(adapter, /spot\.captureRelocationState\(\)/);
  assert.match(adapter, /spot\.restoreRelocationState/);
});
