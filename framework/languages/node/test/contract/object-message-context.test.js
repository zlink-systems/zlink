const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const {
  createInstanceSpotContext,
  createSpotContext
} = require('../../packages/framework/dist/runtime/spots/spot-context');

function contextOptions(overrides = {}) {
  return {
    meshName: 'play',
    spotId: 'room-7',
    objectGeneration: 17,
    handlers: {},
    outbound: {},
    timers: {},
    serial: {},
    getSpot: () => ({}),
    nodeRid: 'node-a',
    workerRuntime: {},
    close: async () => true,
    ...overrides
  };
}

test('Spot contexts expose one immutable identity and user Spot leave ownership', async () => {
  let nodeRid = 'node-a';
  const leaves = [];
  const context = createSpotContext(contextOptions({
    nodeRid: undefined,
    nodeRidProvider: () => nodeRid,
    leaveActor: async (actor, signal) => {
      leaves.push({ actor, signal });
    }
  }));

  nodeRid = 'node-b';
  assert.deepEqual(
    {
      meshName: context.meshName,
      spotId: context.spotId,
      objectGeneration: context.objectGeneration,
      nodeRid: context.nodeRid
    },
    {
      meshName: 'play',
      spotId: 'room-7',
      objectGeneration: 17,
      nodeRid: 'node-a'
    }
  );
  assert.equal('routingId' in context, false);
  for (const key of ['meshName', 'spotId', 'objectGeneration', 'nodeRid']) {
    assert.equal(Object.getOwnPropertyDescriptor(context, key).writable, false);
  }

  const actor = {};
  const controller = new AbortController();
  await context.leaveActor(actor, controller.signal);
  assert.deepEqual(leaves, [{ actor, signal: controller.signal }]);

  const instanceContext = createInstanceSpotContext(contextOptions());
  assert.equal('leaveActor' in instanceContext, false);
  assert.equal('routingId' in instanceContext, false);
});

test('Actor factory receives the sole identity context and mismatched context is rejected', async () => {
  let suppliedContext;
  class ExactFactory {
    async create(context) {
      suppliedContext = context;
      return { context };
    }
  }
  const manager = new framework.DefaultZLinkActorManager({
    actorFactories: new Map([['player', ExactFactory]]),
    actorMeshNameProvider: () => 'play'
  });
  const actor = await manager.getOrCreateActor('player-7', 'player');

  assert.equal(actor.context, suppliedContext);
  assert.equal(actor.context.actorId, 'player-7');
  assert.equal(actor.context.objectGeneration, 1n);
  assert.equal(actor.context.meshName, 'play');
  for (const legacyField of [
    'actorRef',
    'getSpot',
    'handlers',
    'isJoined',
    'joinEntrySpotForRuntime',
    'leaveSpot'
  ]) {
    assert.equal(legacyField in actor.context, false);
  }

  class MismatchedFactory {
    async create(context) {
      return { context: { ...context } };
    }
  }
  const mismatched = new framework.DefaultZLinkActorManager({
    actorFactories: new Map([['player', MismatchedFactory]]),
    actorMeshNameProvider: () => 'play'
  });
  await assert.rejects(
    () => mismatched.getOrCreateActor('player-8', 'player'),
    (error) => error instanceof framework.ZLinkFrameworkException
      && /exact supplied context/.test(error.message)
  );
});

test('Spot Actor dispatch supplies containing Spot and unified MessageContext only', async () => {
  class PlayerActor {}
  class SendHandler {
    async handle(spot, actor, context, message) {
      calls.push({ kind: 'send', spot, actor, context, message });
    }
  }
  class RequestHandler {
    async handle(spot, actor, context, request) {
      calls.push({ kind: 'request', spot, actor, context, request });
      return { ok: true };
    }
  }

  const calls = [];
  const actor = new PlayerActor();
  const spot = {
    context: { meshName: 'play' },
    async onJoinedActor() {},
    async onLeaveActor() {},
    async onDisconnectActor() {}
  };
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime();
  registry.addPacket({
    kind: framework.ZLinkActorPacketKind.Send,
    packetName: 'Notice',
    actorType: PlayerActor,
    handlerType: SendHandler
  });
  registry.addPacket({
    kind: framework.ZLinkActorPacketKind.Request,
    packetName: 'Query',
    actorType: PlayerActor,
    handlerType: RequestHandler
  });
  const dispatcher = new framework.ZLinkSpotActorDispatcher({ registry, spot });
  const legacyFields = {
    meshName: 'play',
    channelName: 'actor',
    contentType: 'application/json',
    metadata: new Map([['trace', '1']]),
    correlationId: 'corr-7',
    connectionAborted: new AbortController().signal,
    routerChannelId: 'legacy',
    reply: { compress() {} }
  };

  await dispatcher.dispatchSend(actor, 'Notice', { value: 1 }, legacyFields);
  assert.deepEqual(
    await dispatcher.dispatchRequest(actor, 'Query', { value: 2 }, legacyFields),
    { ok: true }
  );
  assert.equal(calls.length, 2);
  for (const call of calls) {
    assert.equal(call.spot, spot);
    assert.equal(call.actor, actor);
    assert.deepEqual(
      Object.keys(call.context).sort(),
      ['channelName', 'contentType', 'correlationId', 'meshName', 'metadata', 'packetName']
    );
    assert.equal(call.context.meshName, 'play');
    assert.equal(call.context.correlationId, 'corr-7');
    assert.equal('reply' in call.context, false);
    assert.equal('connectionAborted' in call.context, false);
    assert.equal('routerChannelId' in call.context, false);
  }
});
