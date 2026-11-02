const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const Module = require('node:module');
const esbuild = require('esbuild');
const {
  ZLinkFrameworkException,
  ZLinkFrameworkErrorKind
} = require('../../packages/framework/dist');

function loadRoom() {
  const filename = path.resolve(
    __dirname,
    '../../samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/BingoRoomSpot/bingo-room-spot.ts'
  );
  const output = esbuild.buildSync({
    entryPoints: [filename],
    bundle: true,
    platform: 'node',
    format: 'cjs',
    packages: 'external',
    write: false,
    tsconfig: path.resolve(__dirname, '../../samples/Bingo.Ts/tsconfig.json')
  });
  const loaded = new Module(filename, module);
  loaded.filename = filename;
  loaded.paths = Module._nodeModulePaths(path.dirname(filename));
  loaded._compile(output.outputFiles[0].text, filename);
  return loaded.exports.BingoRoomSpot;
}

function loadHandler() {
  const filename = path.resolve(
    __dirname,
    '../../samples/Bingo.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/Handlers/observe-bingo-events-handler.ts'
  );
  const output = esbuild.buildSync({
    entryPoints: [filename],
    bundle: true,
    platform: 'node',
    format: 'cjs',
    packages: 'external',
    write: false,
    tsconfig: path.resolve(__dirname, '../../samples/Bingo.Ts/tsconfig.json')
  });
  const loaded = new Module(filename, module);
  loaded.filename = filename;
  loaded.paths = Module._nodeModulePaths(path.dirname(filename));
  loaded._compile(output.outputFiles[0].text, filename);
  return loaded.exports.ObserveBingoEventsHandler;
}

test('Bingo observer receives only the finished state snapshot after settlement', async () => {
  const BingoRoomSpot = loadRoom();
  const pushed = [];
  const reward = {
    roomId: 'room',
    actorId: 'winner',
    drawSeq: 15,
    itemId: 'rare-golden-dauber',
    itemName: 'Golden Dauber',
    rarity: 'Legendary'
  };
  const state = { roomId: 'room', status: 'Finished', drawSeq: 15, winners: ['winner'] };
  const room = new BingoRoomSpot(
    {},
    {
      sendToActor: (_actor, message) => ({ submit: async () => pushed.push(message.notification) })
    }
  );
  room.context = {
    spotId: 'observe:room:observer',
    outbound: {
      requestToSpot: (spotId) => {
        assert.equal(spotId, 'room');
        return {
          inMesh() {
            throw new Error('InMesh requires Instance Spot intent');
          },
          submit: async () => ({ state, reward })
        };
      }
    }
  };
  room.initializeRoom({
    mode: 'two-player',
    roomName: 'Observer',
    requiredPlayers: 1,
    maxDrawNumber: 15,
    drawDeck: [],
    purpose: 'Observer',
    observedRoomId: 'room'
  });
  const observer = {
    actorId: 'observer',
    push: async (message) => pushed.push(message),
    context: {
      boundSession: {
        send: (message) => ({ submit: async () => pushed.push(message) })
      }
    }
  };
  room.admitActor('observer', {
    actorId: 'observer',
    roomId: 'room',
    displayName: 'Observer',
    observeOnly: true
  });
  // No event reached this Spot: the room finished before observation was requested.
  await room.onJoinedActor(observer);
  assert.equal(pushed.length, 1);
  assert.deepEqual(pushed[0].state, state);
});

test('Bingo observer arriving after room finish receives no subscription or compensation', async () => {
  const ObserveBingoEventsHandler = loadHandler();
  let createdObserver = false;
  let deferredJoin = false;
  const handler = new ObserveBingoEventsHandler(
    {
      getOrCreate() {
        createdObserver = true;
        throw new Error('finished rooms must not create an observation room');
      }
    },
    {
      requestToSpot: () => ({
        submit: async () => ({
          state: { status: 'Finished' },
          reward: { actorId: 'winner' }
        })
      })
    }
  );
  const response = await handler.handle(
    {},
    {
      actorId: 'observer',
      context: {
        joinSpot: () => ({
          defer() {
            deferredJoin = true;
          }
        })
      }
    },
    {},
    { roomId: 'room' }
  );
  assert.equal(response.subscribed, false);
  assert.equal(createdObserver, false);
  assert.equal(deferredJoin, false);
});

test('Bingo observer request reports a missing room as ended', async () => {
  const ObserveBingoEventsHandler = loadHandler();
  const handler = new ObserveBingoEventsHandler(
    { getOrCreate() { throw new Error('missing room must not create an observer'); } },
    {
      requestToSpot: () => ({
        submit: async () => {
          throw new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NotFound,
            'Room no longer exists'
          );
        }
      })
    }
  );
  const response = await handler.handle({}, { actorId: 'observer' }, {}, { roomId: 'room' });
  assert.equal(response.subscribed, false);
});

test('Bingo player join notification failure is logged and does not stop admission or recipients', async () => {
  const BingoRoomSpot = loadRoom();
  const delivered = [];
  const room = new BingoRoomSpot(
    {},
    {
      sendToActor: (actorId) => ({
        submit: async () => {
          if (actorId === 'disconnected') {
            throw new ZLinkFrameworkException(
              ZLinkFrameworkErrorKind.Unavailable,
              'session is closed'
            );
          }
          delivered.push(actorId);
        }
      })
    }
  );
  room.context = { spotId: 'room' };
  await room.onInitialize();
  room.initializeRoom({
    mode: 'two-player',
    roomName: 'Room',
    requiredPlayers: 3,
    maxDrawNumber: 15,
    drawDeck: [],
    purpose: 'Game'
  });
  room.admitActor('disconnected', {
    actorId: 'disconnected',
    roomId: 'room',
    displayName: 'Disconnected',
    observeOnly: false
  });
  room.admitActor('connected', {
    actorId: 'connected',
    roomId: 'room',
    displayName: 'Connected',
    observeOnly: false
  });
  const joined = room.admitActor('new-player', {
    actorId: 'new-player',
    roomId: 'room',
    displayName: 'New Player',
    observeOnly: false
  });
  room.playerIds.add('disconnected');
  room.playerIds.add('connected');
  const logs = [];
  const originalError = console.error;
  console.error = (...values) => logs.push(values);
  try {
    await room.notifyPlayerJoined(
      'new-player',
      'New Player',
      joined.state.players.at(-1).seat,
      false,
      joined.state
    );
  } finally {
    console.error = originalError;
  }
  assert.equal(joined.state.players.some((player) => player.actorId === 'new-player'), true);
  assert.deepEqual(delivered, ['connected']);
  assert.equal(logs.length, 1);
  assert.match(String(logs[0][0]), /bingo-player-joined-notify-failed room=room recipient=disconnected/);
});

for (const kind of [ZLinkFrameworkErrorKind.NotFound, ZLinkFrameworkErrorKind.Unavailable]) {
  test(`Bingo observer tolerates only a closed room: ${kind}`, async () => {
    const BingoRoomSpot = loadRoom();
    const failure = new ZLinkFrameworkException(kind, 'Room request failed');
    const pushed = [];
    const room = new BingoRoomSpot({}, {});
    room.context = {
      spotId: 'observe:room:observer',
      outbound: {
        requestToSpot: () => ({
          inMesh() {
            throw new Error('InMesh requires Instance Spot intent');
          },
          submit: async () => {
            throw failure;
          }
        })
      }
    };
    room.initializeRoom({
      mode: 'two-player',
      roomName: 'Observer',
      requiredPlayers: 1,
      maxDrawNumber: 15,
      drawDeck: [],
      purpose: 'Observer',
      observedRoomId: 'room'
    });
    room.admitActor('observer', {
      actorId: 'observer',
      roomId: 'room',
      displayName: 'Observer',
      observeOnly: true
    });
    const callback = room.onJoinedActor({
      actorId: 'observer',
      push: async (message) => pushed.push(message)
    });
    if (kind === ZLinkFrameworkErrorKind.NotFound) await callback;
    else await assert.rejects(callback, (error) => error === failure);
    assert.deepEqual(pushed, []);
  });
}
