const assert = require('node:assert/strict');
const childProcess = require('node:child_process');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const test = require('node:test');

const nodeRoot = path.resolve(__dirname, '../..');

test('SupportChat closes a conversation Spot after the close grace deadline', async () => {
  await withCompiledSample(
    'SupportChat.Ts',
    'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts',
    async (outputRoot) => {
      const { ConversationSpot } = require(path.join(
        outputRoot,
        'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.js'
      ));
      const spot = new ConversationSpot(
        { assignNextAgent: () => undefined },
        { get: () => undefined },
        { publish: () => undefined }
      );
      let closeCalls = 0;
      spot.context = {
        close: async () => { closeCalls += 1; return true; },
        leaveActor: async () => undefined
      };
      await spot.onCreate({
        decode: () => ({
          conversationId: 'conversation-1',
          customerActorId: 'customer-1',
          customerDisplayName: 'Customer',
          subject: 'Payment failed'
        })
      });
      spot.assignAgent('agent-1', 'Agent');
      await joinConversationActor(spot, {
        actorId: 'agent-conversation-1',
        participantId: 'agent-1',
        role: 'Agent',
        displayName: 'Agent'
      });

      const idleAt = Date.now() + 3001;
      await spot.onTimer(idleAt);
      await spot.onTimer(idleAt + 1001);

      assert.equal(closeCalls, 1);
    }
  );
});

test('SupportChat explicit close preserves the Spot long enough to reject duplicate close', async () => {
  await withCompiledSample(
    'SupportChat.Ts',
    'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.ts',
    async (outputRoot) => {
      const { ConversationSpot } = require(path.join(
        outputRoot,
        'Server/Support/Infrastructure/ZLink/Spots/ConversationSpot/conversation-spot.js'
      ));
      const spot = new ConversationSpot(
        { assignNextAgent: () => undefined },
        { get: () => undefined },
        { publish: () => undefined }
      );
      let closeCalls = 0;
      spot.context = {
        close: async () => { closeCalls += 1; return true; },
        leaveActor: async () => undefined
      };
      await spot.onCreate({
        decode: () => ({
          conversationId: 'conversation-1',
          customerActorId: 'customer-1',
          customerDisplayName: 'Customer',
          subject: 'Payment failed'
        })
      });
      const customer = {
        actorId: 'customer-1',
        participantId: 'customer-1',
        role: 'Customer',
        displayName: 'Customer'
      };
      await joinConversationActor(spot, customer);

      await spot.close(customer.actorId);

      assert.equal(closeCalls, 0);
      await assert.rejects(async () => spot.close(customer.actorId), /duplicate close/);
    }
  );
});

test('TicTacToe closes a terminal room Spot after every actor leaves', async () => {
  await withCompiledSample(
    'TicTacToe.Ts',
    'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts',
    async (outputRoot) => {
      const { TicTacToeGameSpot } = require(path.join(
        outputRoot,
        'Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.js'
      ));
      const spot = new TicTacToeGameSpot({
        sendToActor: () => ({ submit: async () => ({ status: 'submitted' }) })
      });
      let closeCalls = 0;
      spot.context = {
        spotId: 'room-1',
        close: async () => { closeCalls += 1; return true; }
      };
      await spot.onInitialize();
      const player1 = player('player-1', 'Player 1');
      const player2 = player('player-2', 'Player 2');
      await joinPlayer(spot, player1);
      await joinPlayer(spot, player2);
      await spot.placeMark(player1.actorId, 0);
      await spot.placeMark(player2.actorId, 3);
      await spot.placeMark(player1.actorId, 1);
      await spot.placeMark(player2.actorId, 4);
      await spot.placeMark(player1.actorId, 2);

      await spot.onLeaveActor(player1);
      assert.equal(closeCalls, 0);
      await spot.onLeaveActor(player2);
      assert.equal(closeCalls, 1);
    }
  );
});

async function joinPlayer(spot, actor) {
  const response = await spot.onActorJoin(actor.actorId, {
    decode: () => ({
      roomId: 'room-1',
      player: {
        actorId: actor.actorId,
        displayName: actor.displayName,
        level: actor.level,
        wins: actor.wins
      }
    })
  });
  assert.equal(response.accepted, true);
  await spot.onJoinedActor(actor);
}

async function joinConversationActor(spot, actor) {
  const response = await spot.onActorJoin(actor.actorId, {
    decode: () => ({
      participantId: actor.participantId,
      role: actor.role,
      displayName: actor.displayName
    })
  });
  assert.equal(response.accepted, true);
  await spot.onJoinedActor(actor);
}

function player(actorId, displayName) {
  return {
    actorId,
    displayName,
    level: 10,
    wins: 0,
    push: async () => undefined
  };
}

async function withCompiledSample(sample, entry, run) {
  const outputRoot = fs.mkdtempSync(path.join(os.tmpdir(), `zlink-${sample.toLowerCase()}-`));
  try {
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
        path.join(nodeRoot, 'samples', sample, entry)
      ],
      { cwd: nodeRoot, encoding: 'utf8' }
    );
    assert.equal(result.status, 0, result.stderr || result.stdout);
    fs.symlinkSync(path.join(nodeRoot, 'node_modules'), path.join(outputRoot, 'node_modules'), 'dir');
    await run(outputRoot);
  } finally {
    fs.rmSync(outputRoot, { recursive: true, force: true });
  }
}
