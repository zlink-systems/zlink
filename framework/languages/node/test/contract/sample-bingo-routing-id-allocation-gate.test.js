const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const read = relativePath => fs.readFileSync(path.join(nodeRoot, relativePath), 'utf8');

test('Bingo uses generated routing-id prefixes without slot allocation or fixed Node RID', () => {
  const sources = [
    read('samples/Bingo.Ts/Server/Api/bingo-api-module.ts'),
    read('samples/Bingo.Ts/Server/Play/bingo-play-module.ts'),
    read('samples/Bingo.Ts/Server/Session/bingo-session-module.ts'),
    read('samples/Bingo.Ts/Runner/sample-runner.mjs')
  ];
  assert.match(sources[0], /\.setRoutingIdPrefix\('api'\)/);
  assert.match(sources[1], /\.setRoutingIdPrefix\('play'\)/);
  assert.match(sources[2], /\.setRoutingIdPrefix\('session'\)/);
  for (const source of sources) {
    assert.doesNotMatch(source, /useAllocatedRoutingId|setRoutingIdAllocationGroup|listRoutingIdSlots/);
    assert.doesNotMatch(source, /preferred(?:Owner|Play)?NodeRid|\.setRoutingId\(/);
  }
});

test('Bingo starts the client only after peer and mesh readiness evidence', () => {
  const runner = read('samples/Bingo.Ts/Runner/sample-runner.mjs');
  assert.match(runner, /bingo-ready kind=peer-route node=play-a peer=play-b/);
  assert.match(runner, /bingo-ready kind=peer-route node=play-b peer=play-a/);
  assert.match(runner, /waitMeshReady\(ctx, 'api-a', 'matchmaking'\)/);
  assert.match(runner, /waitMeshReady\(ctx, 'session-b', 'room'\)/);
  assert.match(runner, /ctx\.runBrowser\(/);
  assert.doesNotMatch(runner, /bingo-room-peer ConnectionReady remote=/);
  assert.doesNotMatch(runner, /play-replacement|SIGUSR2|bingo-drain result=drained/);
  assert.doesNotMatch(runner, /WaitingForSlot|routing allocation|slot=|generation=/);
});
