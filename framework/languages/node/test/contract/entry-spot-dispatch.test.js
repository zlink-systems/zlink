const assert = require('node:assert/strict');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');
const spots = require('../../packages/framework/dist/runtime/spots');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');

async function waitFor(condition, label, timeoutMs = 1000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (condition()) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 5));
  }
  throw new Error(`${label} timed out`);
}

function entryActorRuntime(resolveActor) {
  return {
    resolveActor,
    async commitActorTransaction(_actor, onJoined) { await onJoined(); },
    async destroyActor() {},
    async routePacket() {
      return { handled: false };
    }
  };
}

function boundSessionRuntime(overrides = {}) {
  return {
    async receiveRoutedBoundSession() {},
    async receiveRoutedBoundSessionResponse() {},
    async receiveRoutedBoundSessionError() {},
    async receiveRemoteBoundSessionOwnership() {},
    async receiveRemoteBoundSessionSeal() {},
    rememberRemoteBoundSessionTarget() {},
    resolveRemoteBoundSessionTarget() { return undefined; },
    actorPacketTargetForState() { return undefined; },
    async sendActorResponse() {},
    async sendActorError() {},
    ...overrides
  };
}

test('Entry Spot native actor request dispatches to registered handler and replies through actor response sender', async () => {
  const calls = [];
  let dispatchHandler;
  let dispatchError;
  let response;

  class PlayerActor {
    constructor(actorId) {
      this.actorId = actorId;
    }
  }

  class EntrySpot {}

  class MatchHandler {
    async handle(spot, actor, context, request) {
      assert.equal(spot instanceof EntrySpot, true);
      assert.equal(actor.actorId, 'player-1');
      assert.equal(context.packetName, 'Match');
      assert.deepEqual(request, { value: 'ping' });
      assert.equal('reply' in context, false);
      calls.push('handler');
      return { value: 'pong' };
    }
  }

  const actor = new PlayerActor('player-1');
  const nativeSpot = {
    routingId: 'entry-rid',
    setDispatchHandler(handler) {
      dispatchHandler = handler;
    },
    recvRoute() {
      return false;
    },
    async dispose() {}
  };
  const activation = new spots.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    actorRequestHandlers: [{
      entrySpotType: EntrySpot,
      actorType: PlayerActor,
      handlerType: MatchHandler,
      packetName: 'Match'
    }],
    nativeSpot,
    nativeNode: { routingId: 'node-a' },
    nodeRid: 'node-a',
    spotNodeName: 'entry-node',
    detachedTaskRunner: {
      runDetached(_taskName, callback) {
        void callback().catch((error) => {
          dispatchError = error;
        });
      }
    },
    entryActorRuntime: entryActorRuntime((actorId) => actorId === actor.actorId ? actor : undefined),
    boundSessionRuntime: boundSessionRuntime({
      async sendActorResponse(targetActor, packetName, requestSeq, payload, replyOptions) {
        response = {
          actorId: targetActor.actorId,
          packetName,
          requestSeq,
          payload,
          metadata: [...replyOptions.metadata.entries()],
          compressPayload: replyOptions.compressPayload
        };
        calls.push('response');
      }
    })
  });

  await activation.create();
  await activation.configure();
  await activation.initialize();

  const header = zlink.Message.from(Buffer.from(protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 7n,
    name: 'Match',
    metadata: new Map()
  })));
  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));
  const actorRef = {
    nodeRid: zlink.RoutingId.from('node-a'),
    actorId: actor.actorId,
    generation: 1n
  };
  const parts = [
    { info: { actor: actorRef }, message: header, more: true },
    { info: { actor: actorRef }, message: payload, more: false }
  ];

  assert.equal(typeof dispatchHandler, 'function');
  dispatchHandler({
    event: 5,
    recvActorPart() {
      return parts.shift() ?? null;
    }
  });

  await waitFor(() => calls.includes('response') || dispatchError !== undefined, 'Entry Spot actor response');
  if (dispatchError !== undefined) throw dispatchError;
  assert.deepEqual(calls, ['handler', 'response']);
  assert.deepEqual(response, {
    actorId: 'player-1',
    packetName: 'Match',
    requestSeq: 7n,
    payload: { value: 'pong' },
    metadata: [],
    compressPayload: false
  });
});

test('Entry Spot routed actor packet records only an explicit remote bound session target', async () => {
  let capturedTarget;
  let capturedTargetCount = 0;
  const replies = [];
  let dispatchHandler;

  class PlayerActor {
    constructor(actorId) {
      this.actorId = actorId;
    }
  }

  class EntrySpot {}
  class MatchHandler {
    async handle(_spot, actor, _context, request) {
      assert.equal(actor.actorId, 'player-1');
      assert.deepEqual(request, { value: 'ping' });
      return { value: 'pong' };
    }
  }

  const actor = new PlayerActor('player-1');
  const nativeSpot = {
    routingId: 'play-node',
    setDispatchHandler(handler) {
      dispatchHandler = handler;
    },
    recvRoute() {
      return false;
    },
    async dispose() {}
  };
  const activation = new spots.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    actorRequestHandlers: [{
      entrySpotType: EntrySpot,
      actorType: PlayerActor,
      handlerType: MatchHandler,
      packetName: 'Match'
    }],
    nativeSpot,
    nativeNode: { routingId: 'play-node', bindRemoteActorSession() {} },
    nodeRid: 'play-node',
    spotNodeName: 'room',
    entryActorRuntime: entryActorRuntime((actorId) => actorId === actor.actorId ? actor : undefined),
    boundSessionRuntime: boundSessionRuntime({
      rememberRemoteBoundSessionTarget(_actorId, target) {
        capturedTargetCount++;
        capturedTarget = target;
      },
      actorPacketTargetForState(actorId) {
        assert.equal(actorId, 'player-1');
        return {
          routerChannelId: 'bingo.room.route',
          targetNodeRid: zlink.RoutingId.from('play-node-a'),
          spotId: zlink.RoutingId.from('room-1'),
          spotKind: framework.ZLinkSpotKind.User
        };
      }
    })
  });

  await activation.create();
  await activation.configure();
  await activation.initialize();

  const header = zlink.Message.from(Buffer.from(protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 9n,
    name: 'Match',
    metadata: new Map()
  })));
  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));
  const relay = zlink.Message.from(Buffer.from(JSON.stringify({
    packetName: '__zlink.actor.packet.relay',
    actorId: 'player-1',
    routerChannelId: 'bingo.room.route',
    boundSessionTargetNodeRid: 'session-node',
    boundSessionSpotId: 'session-entry',
    header: Buffer.from(header.data()).toString('base64'),
    payload: Buffer.from(payload.data()).toString('base64')
  })));

  dispatchHandler({
    event: 2,
    routed: {
      parts: [relay],
      routingId: 'play-node',
      spotId: 'play-entry',
      requestSeq: 1n,
      reply() {
        return {
          message(message) {
            replies.push(message);
            return this;
          },
          submit() {}
        };
      },
      close() {}
    }
  });

  await waitFor(() => capturedTarget !== undefined, 'remote target capture');
  assert.equal(capturedTarget.routerChannelId, 'bingo.room.route');
  assert.equal(String(capturedTarget.targetNodeRid), 'session-node');
  assert.equal(String(capturedTarget.spotId), 'session-entry');
  assert.equal(typeof capturedTarget.targetNodeRid, 'string');
  assert.equal(typeof capturedTarget.spotId, 'string');
  await waitFor(() => replies.length === 1, 'routed actor packet reply');
  const reply = JSON.parse(Buffer.from(replies[0]).toString('utf8'));
  assert.deepEqual(reply.actorPacketTarget, {
    routerChannelId: 'bingo.room.route',
    targetNodeRid: 'play-node-a',
    targetNodeRidHex: zlink.RoutingId.from('play-node-a').toHex(),
    spotId: 'room-1',
    spotKind: framework.ZLinkSpotKind.User
  });

  const backendRelay = zlink.Message.from(Buffer.from(JSON.stringify({
    packetName: '__zlink.actor.packet.relay',
    actorId: 'player-1',
    routerChannelId: 'bingo.room.route',
    header: Buffer.from(header.data()).toString('base64'),
    payload: Buffer.from(payload.data()).toString('base64')
  })));
  dispatchHandler({
    event: 2,
    routed: {
      parts: [backendRelay],
      routingId: 'backend-node',
      spotId: 'backend-spot',
      requestSeq: 2n,
      reply() {
        return {
          message(message) {
            replies.push(message);
            return this;
          },
          submit() {}
        };
      },
      close() {}
    }
  });
  await waitFor(() => replies.length === 2, 'backend actor packet reply');
  assert.equal(capturedTargetCount, 1);
  backendRelay.close();
  header.close();
  payload.close();
  relay.close();
});

test('Entry Spot materializes a remotely returning actor with its original Entry node', async () => {
  let dispatchHandler;
  const replies = [];
  const events = [];
  const originalEntryNodeRid = zlink.RoutingId.from('play-node-b');
  const actorRef = {
    nodeRid: originalEntryNodeRid,
    actorId: 'player-2',
    generation: 2n
  };
  const actor = {
    context: {
      actorId: 'player-2',
      [framework.ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]() {
        return {
          actorRef,
          actorType: 'PlayerActor',
          membershipEpoch: 1n
        };
      }
    }
  };

  class EntrySpot {
    async onJoinedActor(joinedActor) {
      events.push(`joined:${joinedActor.context.actorId}`);
    }
  }

  const activation = new spots.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot: {
      routingId: 'play-node-b-entry',
      setDispatchHandler(handler) {
        dispatchHandler = handler;
      },
      recvRoute() {
        return false;
      },
      async dispose() {}
    },
    nativeNode: { routingId: originalEntryNodeRid, bindRemoteActorSession() {} },
    nodeRid: originalEntryNodeRid,
    spotNodeName: 'play-b',
    entryActorRuntime: {
      resolveActor() { return undefined; },
      async commitActorTransaction(_actor, onJoined) {
        events.push('commit');
        try {
          await onJoined();
        } catch (error) {
          events.push(`commit-error:${error.message}`);
          throw error;
        }
      },
      async destroyActor() {},
      async routePacket() { return { handled: false }; }
    },
    actorTransferRuntime: {
      async materializeRoutedActor(actorId, actorType, adapterKey, state, actorEntryNodeRid) {
        events.push(
          `materialize:${actorId}:${actorType}:${adapterKey}:${state.data().toString()}:${String(actorEntryNodeRid)}`
        );
        assert.equal(String(actorEntryNodeRid), String(originalEntryNodeRid));
        return {
          actor,
          actorRef
        };
      }
    }
  });

  await activation.create();
  await activation.configure();
  await activation.initialize();

  const common = {
    packetName: '__zlink.actor.join_spot.request',
    actorId: 'player-2',
    actorType: 'PlayerActor',
    actorNodeRid: 'play-node-a',
    actorGeneration: '1',
    actorEntryNodeRid: String(originalEntryNodeRid),
    actorEntryNodeRidHex: originalEntryNodeRid.toHex(),
    request: Buffer.from(JSON.stringify({ returnHome: true })).toString('base64'),
    transferId: 'return-player-2'
  };
  const route = (payload) => {
    const part = zlink.Message.from(Buffer.from(JSON.stringify(payload)));
    dispatchHandler({
      event: 2,
      routed: {
        parts: [part],
        routingId: 'play-node-a',
        spotId: 'play-node-b-entry',
        requestSeq: 1n,
        reply() {
          return {
            message(message) {
              replies.push(JSON.parse(Buffer.from(message).toString('utf8')));
              return this;
            },
            submit() {}
          };
        },
        close() {
          part.close();
        }
      }
    });
  };

  route({ ...common, phase: 'admission' });
  await waitFor(() => replies.length === 1, 'Entry Spot remote admission reply');
  assert.equal(replies[0].accepted, true);

  route({
    ...common,
    phase: 'commit',
    transferAdapterKey: 'PlayerActor',
    transferState: Buffer.from('player-state').toString('base64')
  });
  await waitFor(() => replies.length === 2, 'Entry Spot remote commit reply');
  assert.equal(replies[1].accepted, true, JSON.stringify({ reply: replies[1], events }));
  assert.deepEqual(events, [
    'materialize:player-2:PlayerActor:PlayerActor:player-state:play-node-b',
    'commit',
    'joined:player-2'
  ]);

  await activation.dispose();
});

test('Entry Spot routed bound session command decodes registered channel serializer', async () => {
  let dispatchHandler;
  let received;
  const contentType = 'application/x-bound-session-test';
  const serializer = {
    deserialize(payload) {
      assert.equal(Buffer.from(payload.data()).toString('utf8'), 'encoded-bound-session');
      return {
        actorId: 'player-1',
        message: { hello: 'world' },
        boundPacketName: 'Notify',
        metadata: { trace: 'yes' }
      };
    }
  };

  class EntrySpot {}

  const nativeSpot = {
    routingId: 'session-entry',
    setDispatchHandler(handler) {
      dispatchHandler = handler;
    },
    recvRoute() {
      return false;
    },
    async dispose() {}
  };
  const activation = new spots.ZLinkEntrySpotActivation({
    entrySpotType: EntrySpot,
    nativeSpot,
    nativeNode: { routingId: 'session-node' },
    nodeRid: 'session-node',
    spotNodeName: 'session',
    messageSerializers: new Map([[contentType, serializer]]),
    boundSessionRuntime: boundSessionRuntime({
      async receiveRoutedBoundSession(actorId, message, packetName, metadata) {
        received = { actorId, message, packetName, metadata: [...metadata.entries()] };
      }
    })
  });

  await activation.create();
  await activation.configure();
  await activation.initialize();

  const header = zlink.Message.from(Buffer.from(JSON.stringify({
    formatMarker: 0xf2,
    flowId: '018f2b63-9d4a-7abc-8def-0123456789ab',
    flowOrigin: 1,
    kind: 3,
    channelName: 'bingo.room.route',
    messageName: '__zlink.actor.bound_session.send',
    contentType,
    correlationId: null,
    deadline: null,
    topic: null,
    errorCode: null,
    errorMessage: null
  })));
  const payload = zlink.Message.from(Buffer.from('encoded-bound-session'));

  dispatchHandler({
    event: 2,
    routed: {
      parts: [header, payload],
      routingId: 'play-node',
      spotId: 'session-entry',
      requestSeq: null,
      reply() {
        throw new Error('send command is not replyable');
      },
      close() {}
    }
  });

  await waitFor(() => received !== undefined, 'routed bound session command');
  assert.deepEqual(received, {
    actorId: 'player-1',
    message: { hello: 'world' },
    packetName: 'Notify',
    metadata: [['trace', 'yes']]
  });
  header.close();
  payload.close();
});

test('runtime host reports joined Spot route before stale remote actor packet target', () => {
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: { useInMemoryStores: true },
      routeChannels: ['bingo.room.route']
    })
  });
  runtime.actorManager = {
    getState(actorId) {
      assert.equal(actorId, 'player-2');
      return {
        spotId: 'bingo-room-1',
        nativeActorRef: {
          nodeRid: 'play-node-1',
          actorId: 'player-2',
          generation: 1n
        },
        remoteActorPacketTarget: {
          routerChannelId: 'bingo.room.route',
          targetNodeRid: 'play-node-1',
          spotId: 'play-entry-spot'
        }
      };
    }
  };

  assert.deepEqual(runtime.boundSessionRelay.actorPackets.actorPacketTargetForState('player-2'), {
    routerChannelId: 'bingo.room.route',
    targetNodeRid: 'play-node-1',
    spotId: 'bingo-room-1',
    spotKind: 'user'
  });
});

test('runtime host normalizes remote actor join bound-session route ids', async () => {
  let capturedTarget;
  const actor = { actorId: 'player-1' };
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({})
  });
  runtime.actorManager = {
    async getOrCreateActor(actorId) {
      assert.equal(actorId, 'player-1');
      return actor;
    },
    getState(actorId) {
      assert.equal(actorId, 'player-1');
      return {
        setNativeActorRef() {},
        setRemoteBoundSessionTarget(target) {
          capturedTarget = target;
        },
        setJoinedSpot() {}
      };
    }
  };
  runtime.spotManager = {
    async admitActorJoin(_spotId, joinedActor, request, commit) {
      assert.equal(joinedActor, actor);
      assert.equal(request.data().toString(), 'join-request');
      commit({});
      return { accepted: true };
    }
  };

  const result = await runtime.boundSessionRelay.actorJoins.receive({
    packetName: '__zlink.actor.join_spot.request',
    spotId: 'room-1',
    actorId: 'player-1',
    actorType: 'PlayerActor',
    actorNodeRid: 'play-node',
    actorGeneration: '1',
    routerChannelId: 'bingo.room.route',
    request: Buffer.from('join-request').toString('base64')
  }, {
    channelName: 'bingo.room.route',
    sourceNodeRid: 'session-node'
  });

  assert.equal(result.accepted, true);
  assert.equal(capturedTarget.routerChannelId, 'bingo.room.route');
  assert.equal(String(capturedTarget.targetNodeRid), 'session-node');
  assert.equal(String(capturedTarget.spotId), 'session-node');
  assert.equal(typeof capturedTarget.targetNodeRid, 'string');
  assert.equal(typeof capturedTarget.spotId, 'string');
});

test('runtime host remembers routed packet target for stream-bound actors without actor manager', async () => {
  const routedTargets = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: { useInMemoryStores: true },
      routeChannels: ['bingo.room.route']
    })
  });
  runtime.routeTransport.requestRawToSpot = async (remoteAddress) => {
    routedTargets.push(`${remoteAddress.routerChannelId}:${remoteAddress.targetNodeRid}`);
    return [zlink.Message.from(Buffer.from(JSON.stringify({
      ok: true,
      response: { accepted: true },
      actorPacketTarget: {
        routerChannelId: 'bingo.room.route',
        targetNodeRid: 'play-node-1',
        spotId: 'bingo-room-1'
      }
    })))];
  };
  runtime.streamBindingRuntime.sendLocalBoundSessionResponse = () => true;

  const actor = {
    actorId: 'player-2',
    ref: {
      nodeRid: 'play-node-1',
      actorId: 'player-2',
      objectGeneration: 1n,
      meshName: 'bingo.room.route'
    }
  };
  const header = {
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 1n,
    name: 'Match',
    metadata: { values: new Map() }
  };
  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));

  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(actor, header, payload);
  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(actor, { ...header, requestSeq: 2n, name: 'Submit' }, payload);

  payload.close();
  assert.deepEqual(routedTargets, ['bingo.room.route:play-node-1', 'bingo.room.route:play-node-1']);
});

test('runtime host keeps routed packet target across stream actor wrappers', async () => {
  const routedTargets = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: { useInMemoryStores: true },
      routeChannels: ['bingo.room.route']
    })
  });
  runtime.routeTransport.requestRawToSpot = async (remoteAddress) => {
    routedTargets.push(`${remoteAddress.routerChannelId}:${remoteAddress.targetNodeRid}:${remoteAddress.spotId}`);
    return [zlink.Message.from(Buffer.from(JSON.stringify({
      ok: true,
      response: { accepted: true },
      actorPacketTarget: {
        routerChannelId: 'bingo.room.route',
        targetNodeRid: 'play-node-a',
        spotId: 'room-1',
        spotKind: framework.ZLinkSpotKind.User
      }
    })))];
  };
  runtime.streamBindingRuntime.sendLocalBoundSessionResponse = () => true;

  const actorRef = {
    nodeRid: 'play-node-b',
    actorId: 'player-2',
    objectGeneration: 1n,
    meshName: 'bingo.room.route'
  };
  const header = {
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 1n,
    name: 'Match',
    metadata: { values: new Map() }
  };
  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));

  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket({ actorId: 'player-2', ref: actorRef }, header, payload);
  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(
    { actorId: 'player-2', ref: actorRef },
    { ...header, requestSeq: 2n, name: 'Submit' },
    payload
  );

  payload.close();
  assert.deepEqual(routedTargets, [
    'bingo.room.route:play-node-b:play-node-b',
    'bingo.room.route:play-node-a:room-1'
  ]);
});

test('runtime host raw actor relay reply updates actor packet target for the next request', async () => {
  const routedTargets = [];
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      locations: { useInMemoryStores: true },
      routeChannels: ['bingo.room.route']
    })
  });
  runtime.spotNodeRuntime = {
    primaryNode: {
      routingId: 'bingo-session-node-b'
    }
  };
  const state = new framework.ZLinkActorRuntimeState('player-2');
  state.setRemoteActorPacketTarget({
    routerChannelId: 'bingo.room.route',
    targetNodeRid: 'bingo-play-node-b',
    spotId: 'bingo-play-node-b',
    spotKind: framework.ZLinkSpotKind.Entry
  });
  runtime.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'player-2');
      return state;
    }
  });
  runtime.routeTransport.requestRawToSpot = async (remoteAddress) => {
    routedTargets.push(`${remoteAddress.routerChannelId}:${remoteAddress.targetNodeRid}:${remoteAddress.spotId}`);
    return [zlink.Message.from(Buffer.from(JSON.stringify({
      ok: true,
      response: { accepted: true },
      actorPacketTarget: {
        routerChannelId: 'bingo.room.route',
        targetNodeRid: 'bingo-play-node-a',
        spotId: 'bingo-room-1',
        spotKind: framework.ZLinkSpotKind.User
      }
    })))];
  };
  runtime.streamBindingRuntime.sendLocalBoundSessionResponse = () => true;

  const actor = {
    actorId: 'player-2',
    ref: {
      nodeRid: 'bingo-play-node-b',
      actorId: 'player-2',
      objectGeneration: 1n,
      meshName: 'bingo.room.route'
    }
  };
  const header = {
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 1n,
    name: 'MatchBingoReq',
    metadata: { values: new Map() }
  };
  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));

  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(actor, header, payload);
  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(
    actor,
    { ...header, requestSeq: 2n, name: 'SubmitBingoCardReq' },
    payload
  );

  payload.close();
  assert.deepEqual(routedTargets, [
    'bingo.room.route:bingo-play-node-b:bingo-play-node-b',
    'bingo.room.route:bingo-play-node-a:bingo-room-1'
  ]);
  assert.deepEqual({
    routerChannelId: state.remoteActorPacketTarget.routerChannelId,
    targetNodeRid: String(state.remoteActorPacketTarget.targetNodeRid),
    spotId: String(state.remoteActorPacketTarget.spotId),
    spotKind: state.remoteActorPacketTarget.spotKind
  }, {
    routerChannelId: 'bingo.room.route',
    targetNodeRid: 'bingo-play-node-a',
    spotId: 'bingo-room-1',
    spotKind: framework.ZLinkSpotKind.User
  });
});
