const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

test('TicTacToe uses only the framework location store for room routing', () => {
  const sampleRoot = path.join(nodeRoot, 'samples/TicTacToe.Ts');
  const endpoint = fs.readFileSync(path.join(
    sampleRoot,
    'Server/Api/Handlers/create-game-http-handler.ts'
  ), 'utf8');
  const obsoleteStore = path.join(sampleRoot, 'Server/Configuration/redis-room-route-store.ts');

  assert.equal(fs.existsSync(obsoleteStore), false);
  assert.match(endpoint, /\.create\(SampleNames\.gameSpotType\)/);
  assert.match(endpoint, /\.inMesh\(SampleNames\.playSpotNode\)/);
  assert.doesNotMatch(endpoint, /requestToChannel|ownerPlayEndpoint|NodeRid/);
});
