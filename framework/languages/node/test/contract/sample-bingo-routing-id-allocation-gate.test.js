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

test('Bingo rolling replacement uses readiness and drain evidence', () => {
  const runner = read('samples/Bingo.Ts/Runner/sample-runner.mjs');
  assert.match(runner, /play-replacement/);
  assert.match(runner, /SIGUSR2/);
  assert.match(runner, /bingo-drain result=drained/);
  assert.match(runner, /bingo-room-status state=1 readyPeers=/);
  assert.match(read('samples/Bingo.Ts/Server/Configuration/room-router-readiness-handler.ts'), /placement=\$\{status\.placement\.isAvailable\}/);
  assert.doesNotMatch(runner, /bingo-room-peer ConnectionReady remote=/);
  assert.doesNotMatch(runner, /WaitingForSlot|routing allocation|slot=|generation=/);
});
