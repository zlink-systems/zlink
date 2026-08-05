const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const runner = fs.readFileSync(
  path.resolve(__dirname, '../../samples/Bingo.Ts/Runner/sample-runner.mjs'),
  'utf8'
);
const session = fs.readFileSync(
  path.resolve(__dirname, '../../samples/Bingo.Ts/Server/Session/Sessions/bingo-session.ts'),
  'utf8'
);

test('Bingo runner proves room leave and actor destroy lifecycle boundaries', () => {
  for (const actorId of ['player-1', 'player-2']) {
    assert.match(runner, new RegExp(`bingo-lifecycle room-leave actor=${actorId}`));
  }
  assert.match(runner, /waitPlayLog\(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-1'\)/);
  assert.match(runner, /waitPlayLog\(ctx, 'bingo-lifecycle entry-destroy-complete actor=player-2'\)/);
  assert.match(runner, /assertPlayLogCount\(ctx, 'bingo-lifecycle entry-leave actor=player-1', 1\)/);
  assert.match(runner, /assertPlayLogCount\(ctx, 'bingo-lifecycle entry-leave actor=player-2', 1\)/);
  assert.match(runner, /assertPlayLogCount\(ctx, 'bingo-record reported actor=observer', 0\)/);
  assert.match(runner, /bingo-lifecycle session-disconnect actor=player-1 destroy=false/);
  assert.match(runner, /bingo-lifecycle session-disconnect actor=player-2 destroy=false/);

  const disconnected = session.match(/async onDisconnected\(\)[\s\S]*?\n  }/)?.[0] ?? '';
  assert.match(disconnected, /bingo-lifecycle session-disconnect/);
  assert.doesNotMatch(disconnected, /destroyActor|markForDestroy/);
});
