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

test('Bingo observer receives the finished snapshot and reward when membership commits', async () => {
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
  assert.equal(pushed.length, 2);
  assert.deepEqual(pushed[0].state, state);
  assert.equal(pushed[1].actorId, 'winner');
  await room.announceReward(reward);
  assert.equal(pushed.length, 2);
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
