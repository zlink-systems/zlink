const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const sampleRoot = path.join(nodeRoot, 'samples/TicTacToe.Ts');

test('TicTacToe turn timeout is not a win and preserves the last completed move', () => {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-tictactoe-match-'));
  try {
    compileMatch(outputRoot);
    const { TicTacToeMatch } = require(path.join(
      outputRoot,
      'Server/Play/Domain/TicTacToe/tictactoe-match.js'
    ));
    const { GameStatus } = require(path.join(outputRoot, 'Shared/Contracts/messages.js'));
    const match = new TicTacToeMatch('room-1', 1);
    match.joinPlayer({ actorId: 'player-x' });
    match.joinPlayer({ actorId: 'player-o' });
    match.placeMark('player-x', 0);

    const result = match.tick(Number.MAX_SAFE_INTEGER);

    assert.equal(result.changed, true);
    assert.equal(result.state.status, GameStatus.TurnTimedOut);
    assert.equal(result.state.winner, null);
    assert.equal(result.state.lastMoveActorId, 'player-x');
    assert.equal(result.state.lastMoveCell, 0);
  } finally {
    fs.rmSync(outputRoot, { recursive: true, force: true });
  }
});

function compileMatch(outputRoot) {
  const result = childProcess.spawnSync(
    process.execPath,
    [
      path.join(nodeRoot, 'node_modules/typescript/bin/tsc'),
      '--target',
      'ES2022',
      '--module',
      'commonjs',
      '--moduleResolution',
      'node',
      '--skipLibCheck',
      '--outDir',
      outputRoot,
      '--rootDir',
      sampleRoot,
      path.join(sampleRoot, 'Server/Play/Domain/TicTacToe/tictactoe-match.ts')
    ],
    { cwd: nodeRoot, encoding: 'utf8' }
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
}
