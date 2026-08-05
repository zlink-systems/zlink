const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');
const sampleRoot = path.join(nodeRoot, 'samples/TicTacToe.Ts');

test('TicTacToe relays ObserveMilestoneReq to the bound actor', async () => {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-tictactoe-session-'));
  try {
    compileSession(outputRoot);
    fs.symlinkSync(path.join(nodeRoot, 'node_modules'), path.join(outputRoot, 'node_modules'), 'dir');
    const { PlaySession } = require(path.join(
      outputRoot,
      'Server/Play/Infrastructure/ZLink/Sessions/play-session.js'
    ));
    let relayCalls = 0;
    let orphanEntrySpotCalls = 0;
    let authenticated = false;
    const sessionActor = {
      relay: async (payload) => {
        assert.equal(payload, observePayload);
        relayCalls += 1;
      },
      notifyDisconnected: async () => undefined
    };
    const context = {
      actors: {
        get bound() { return authenticated ? [sessionActor] : []; },
        bindOrGet: async () => sessionActor,
        find: () => sessionActor
      },
      handlers: {
        addHandler: () => undefined,
        tryHandle: async (dispatch) => {
          if (dispatch.packetName !== 'AuthenticateReq') return false;
          authenticated = true;
          return true;
        }
      },
      client: {
        reply: () => ({ submit: () => undefined }),
        send: () => ({
          metadata: () => ({ submit: async () => undefined })
        })
      }
    };
    const session = new PlaySession(context);

    await session.onDispatch(
      { packetName: 'AuthenticateReq' },
      { decode: () => ({ accessToken: 'player-1-token' }) }
    );
    const observePayload = { decode: () => ({}) };
    await session.onDispatch({ packetName: 'ObserveMilestoneReq' }, observePayload);

    assert.equal(relayCalls, 1);
    assert.equal(orphanEntrySpotCalls, 0);
    const factory = fs.readFileSync(path.join(
      sampleRoot,
      'Server/Play/Infrastructure/ZLink/Sessions/play-session-factory.ts'
    ), 'utf8');
    assert.doesNotMatch(factory, /new PlayEntrySpot/);
  } finally {
    fs.rmSync(outputRoot, { recursive: true, force: true });
  }
});

function compileSession(outputRoot) {
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
      '--experimentalDecorators',
      '--skipLibCheck',
      '--outDir',
      outputRoot,
      path.join(sampleRoot, 'Server/Play/Infrastructure/ZLink/Sessions/play-session.ts')
    ],
    { cwd: nodeRoot, encoding: 'utf8' }
  );
  assert.equal(result.status, 0, result.stderr || result.stdout);
}
