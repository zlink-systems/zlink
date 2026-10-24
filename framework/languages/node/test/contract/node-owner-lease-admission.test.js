'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { ZLinkSubmitStatus } = require('../../packages/framework/dist/runtime/messaging/submission-result');

function scenario(t, ownerUsable) {
  const registration = framework.createFrameworkRegistrationWithBuilder(builder => {
    builder.addRouteMesh('mesh').objects().server();
  });
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  host.admission.register('mesh');
  let ownerReads = 0;
  let dispatched = 0;
  host.locationOwner.runtime = {
    get ownerLeaseUsable() { ownerReads++; return ownerUsable; }
  };
  const dispatch = () => { dispatched++; };
  const dispatchAdmitted = (...args) => args.at(-1)(dispatch);
  host.channelRuntime = {
    dispatchMeshRoute: dispatchAdmitted,
    dispatchMeshChannel: dispatchAdmitted,
    canDispatchLocalMeshRoute: () => true,
    dispatchLocalMeshRoute: dispatch
  };
  host.spotManager = { dispatchMeshActor: dispatchAdmitted, dispatchMeshInstance: dispatchAdmitted };
  t.mock.method(host.serviceRelocation, 'tryHandleControl', () => false);
  t.mock.method(host, 'dispatchMeshSpotRecord', dispatchAdmitted);
  return { host, ownerReads: () => ownerReads, dispatched: () => dispatched, setOwnerUsable: value => { ownerUsable = value; } };
}

const directKinds = ['NodeSend', 'NodeRequest', 'ChannelSend', 'ChannelRequest'];
for (const kind of directKinds) {
  for (const ownerUsable of [true, false]) {
    test(`${kind} dispatch does not query Location owner (usable=${ownerUsable})`, async t => {
      const s = scenario(t, ownerUsable);
      for (let index = 0; index < 4; index++) {
        await s.host.dispatchMeshRecord(() => 'remote', 'mesh', {}, {
          kind: framework.ReceiveKind[kind], domain: framework.ReadyDomain.Application, parts: []
        });
      }
      assert.equal(s.dispatched(), 4);
      assert.equal(s.ownerReads(), 0);
      assert.equal(s.host.admission.pending('mesh'), 0);
      s.host.admission.seal('mesh');
      await assert.rejects(s.host.dispatchMeshRecord(() => 'remote', 'mesh', {}, {
        kind: framework.ReceiveKind[kind], domain: framework.ReadyDomain.Application, parts: []
      }), /admission is closed/u);
      assert.equal(s.dispatched(), 4);
      assert.equal(s.ownerReads(), 0);
    });
  }
}

for (const kind of ['ActorSend', 'ActorRequest', 'SpotSend', 'SpotRequest', 'SpotMulticast', 'InstanceSpotActivation']) {
  test(`${kind} dispatch still requires Location owner`, async t => {
    const s = scenario(t, false);
    await assert.rejects(s.host.dispatchMeshRecord(() => 'remote', 'mesh', {}, {
      kind: framework.ReceiveKind[kind], domain: framework.ReadyDomain.Application, parts: []
    }), /admission is closed/u);
    assert.equal(s.dispatched(), 0);
    assert(s.ownerReads() > 0);
    assert.equal(s.host.admission.pending('mesh'), 0);
  });
}

test('infrastructure Node dispatch remains independent of owner admission and mesh seal', async t => {
  const s = scenario(t, false);
  s.host.admission.seal('mesh');
  await s.host.dispatchMeshRecord(() => 'remote', 'mesh', {}, {
    kind: framework.ReceiveKind.NodeSend, domain: framework.ReadyDomain.Infrastructure, parts: []
  });
  assert.equal(s.dispatched(), 1);
  assert.equal(s.ownerReads(), 0);
});

test('local Node submit skips Location owner and still obeys mesh seal', async t => {
  const s = scenario(t, false);
  const tasks = [];
  s.host.executionState = {
    abortController: new AbortController(),
    taskRunner: { runDetached: (_name, task) => tasks.push(task) }
  };
  let released = 0;
  t.mock.method(s.host.applicationJobQueue, 'acquire', async () => ({
    markApplicationQueued() {},
    releaseAfterInternalProcessing() { released++; }
  }));
  assert.equal(s.host.submitLocalMeshRoute('mesh', 'source', []).status,
    ZLinkSubmitStatus.Submitted);
  assert.equal(s.host.admission.pending('mesh'), 1);
  await tasks.shift()();
  assert.equal(s.dispatched(), 1);
  assert.equal(released, 1);
  assert.equal(s.host.admission.pending('mesh'), 0);
  assert.equal(s.ownerReads(), 0);
  s.host.admission.seal('mesh');
  assert.equal(s.host.submitLocalMeshRoute('mesh', 'source', []).status,
    ZLinkSubmitStatus.Shutdown);
  assert.equal(tasks.length, 0);
  assert.equal(s.ownerReads(), 0);
});

const relayWire = require('../../packages/framework/dist/runtime/actors/actor-packet-relay-wire');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');

const { ZLinkActorPacketKind, ZLinkSpotActorHandlerRegistryRuntime } = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
const { ZLinkBufferMessage } = require('../../packages/framework/dist/runtime/backend/runtime-message');

async function actorEndpoint(t, s, entry) {
  let actorCalls = 0;
  class Actor { constructor() { this.context = { actorId: 'actor' }; } }
  class Spot {}
  class Handler { async handle() { actorCalls++; return 'reply'; } }
  const actor = new Actor();
  let dispatch;
  let execute;
  let observeAdmission;
  let admissionExecutor;
  let admissionMethod;
  if (entry) {
    const options = s.host.createSpotNodeRuntimeOptions(undefined,
      s.host.createDispatchErrorReporter(s.host.runtimeOrPreStartErrorSink));
    const manager = new framework.ZLinkSpotNodeRuntimeManager({
      ...options,
      entryActorRuntime: { resolveActor: () => actor, routePacket: () => undefined }
    });
    const nativeSpot = { routingId: 'entry', setDispatchHandler() {}, async dispose() {} };
    await manager.initializeEntrySpot('mesh', {
      entrySpot: () => nativeSpot,
      status: () => ({ routingId: 'node', lifecycleGeneration: 1n })
    }, {
      entrySpotType: Spot,
      entrySpotActorRequestHandlers: [{ entrySpotType: Spot, actorType: Actor, handlerType: Handler, packetName: 'Probe' }]
    });
    const activation = manager.entryActivations.get('mesh');
    t.after(() => activation.dispose());
    dispatch = (origin, _actorId, parts, returnResponse) => activation.dispatchActorPacket({ origin, actorId: 'actor', parts, returnResponse });
    execute = operation => activation.executeActor('actor', operation);
    admissionExecutor = activation.spotSerialExecutor;
    admissionMethod = 'executeActor';
  } else {
    const registry = new ZLinkSpotActorHandlerRegistryRuntime();
    registry.addPacket({ kind: ZLinkActorPacketKind.Request, packetName: 'Probe', actorType: Actor, handlerType: Handler });
    const serial = new framework.ZLinkSpotSerialTurnExecutor(true, 'actor');
    t.after(async () => { serial.closeAdmission(); await serial.whenIdle(); });
    const activation = {
      meshName: 'mesh', spotId: 'room', spot: { context: { meshName: 'mesh' } },
      actorHandlers: registry, hasDepartedActor: () => false,
      resolveJoinedActor: () => actor,
      executeActor: (_id, operation) => operation(serial)
    };
    const coordinator = new framework.ZLinkSpotActorAdmissionCoordinator({ admission: s.host.admission });
    dispatch = (...args) => coordinator.dispatchActorPacketDirect(args[0], activation, ...args.slice(1));
    execute = operation => serial.execute(operation);
    admissionExecutor = serial;
    admissionMethod = 'execute';
  }
  const admit = admissionExecutor[admissionMethod].bind(admissionExecutor);
  t.mock.method(admissionExecutor, admissionMethod, (...args) => {
    const pending = admit(...args);
    observeAdmission?.();
    observeAdmission = undefined;
    return pending;
  });
  return { dispatch, execute, actorCalls: () => actorCalls,
    nextAdmission: () => new Promise(resolve => { observeAdmission = resolve; }) };
}

function actorParts() {
  return [new ZLinkBufferMessage(protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 1n,
    name: 'Probe',
    metadata: new Map()
  })), new ZLinkBufferMessage(Buffer.from('{}'))];
}

for (const entry of [true, false]) {
  for (const ownerUsable of [true, false]) {
    test(`Node Actor relay checks owner at actual callback (entry=${entry}, usable=${ownerUsable})`, async t => {
      const s = scenario(t, ownerUsable);
      const endpoint = await actorEndpoint(t, s, entry);
      s.host.actorManager = { getState: () => entry ? undefined : { spotId: 'room' } };
      s.host.spotNodeRuntime = { dispatchEntryActorPacket: endpoint.dispatch };
      s.host.spotManager.dispatchRoutedActorPacket = (origin, _spot, ...args) => endpoint.dispatch(origin, ...args);
      const parts = actorParts();
      const payload = relayWire.encodeRemoteActorPacketRelayPayload({
        actorId: 'actor', header: parts[0].data(), payload: parts[1].data(), returnResponse: true
      });
      parts.forEach(part => part.close());
      let result;
      s.host.channelRuntime.dispatchMeshRoute = (_mesh, _record, _signal, admit) => admit(async () => {
        result = await s.host.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(payload, {
          meshName: 'mesh', sourceNodeRid: 'source'
        });
      });
      await s.host.dispatchMeshRecord(() => 'remote', 'mesh', {}, {
        kind: framework.ReceiveKind.NodeRequest, domain: framework.ReadyDomain.Application, parts: []
      });
      assert.equal(result.ok, ownerUsable);
      assert.equal(endpoint.actorCalls(), ownerUsable ? 1 : 0);
      assert.equal(s.ownerReads(), 1);
      assert.equal(s.host.admission.pending('mesh'), 0);
    });
  }
  for (const closeOwner of [true, false]) {
    test(`accepted Actor callback rechecks owner without reopening mesh admission (entry=${entry}, closeOwner=${closeOwner})`, async t => {
      const s = scenario(t, true);
      const endpoint = await actorEndpoint(t, s, entry);
      let release;
      let entered;
      const started = new Promise(resolve => { entered = resolve; });
      const held = new Promise(resolve => { release = resolve; });
      const holding = endpoint.execute(async () => { entered(); await held; });
      let parts;
      let pending;
      try {
        await started;
        parts = actorParts();
        const queued = endpoint.nextAdmission();
        pending = endpoint.dispatch('local', 'actor', parts, true);
        await queued;
        assert.equal(endpoint.actorCalls(), 0);
        if (closeOwner) s.setOwnerUsable(false);
        else s.host.admission.seal('mesh');
        release();
        if (closeOwner) await assert.rejects(pending, /owner admission is closed/u);
        else assert.equal(await pending, 'reply');
        assert.equal(endpoint.actorCalls(), closeOwner ? 0 : 1);
        assert.equal(s.ownerReads(), 1);
      } finally {
        release();
        await holding;
        if (pending !== undefined) await Promise.allSettled([pending]);
        parts?.forEach(part => part.close());
      }
    });
  }
}

test('Actor relay bind acknowledgement stays independent of owner lease', async t => {
  const s = scenario(t, false);
  s.host.actorManager = { getState: () => ({ nativeActorRef: { actorId: 'actor' } }) };
  const payload = relayWire.encodeRemoteActorPacketRelayPayload({
    actorId: 'actor',
    header: protocol.encodeStreamHeader({
      kind: protocol.ZLinkStreamMessageKind.Send,
      codec: protocol.ZLinkStreamCodec.Raw,
      flags: protocol.ZLinkStreamHeaderFlags.None,
      name: relayWire.ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET,
      metadata: new Map()
    }),
    payload: relayWire.encodeRemoteActorSessionBinding({ sessionNodeRid: 'source', sessionRid: 'session' })
  });
  const result = await s.host.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(payload, {
    meshName: 'mesh', sourceNodeRid: 'source'
  });
  assert.equal(result.ok, true);
  assert.equal(s.ownerReads(), 0);
});

test('Actor relay disconnect cleanup survives expired owner and mesh seal', async t => {
  const s = scenario(t, false);
  s.host.admission.seal('mesh');
  s.host.actorManager = { getState: () => undefined };
  let disconnects = 0;
  s.host.spotNodeRuntime = { dispatchEntryActorPacket: async () => { disconnects++; } };
  const payload = relayWire.encodeRemoteActorPacketRelayPayload({
    actorId: 'actor',
    header: protocol.encodeStreamHeader({
      kind: protocol.ZLinkStreamMessageKind.Send,
      codec: protocol.ZLinkStreamCodec.Raw,
      flags: protocol.ZLinkStreamHeaderFlags.None,
      name: relayWire.ZLINK_REMOTE_ACTOR_SESSION_DISCONNECTED_PACKET,
      metadata: new Map()
    }),
    payload: Buffer.alloc(0)
  });
  const result = await s.host.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(payload, {
    meshName: 'mesh', sourceNodeRid: 'source'
  });
  assert.equal(result.ok, true);
  assert.equal(disconnects, 1);
  assert.equal(s.ownerReads(), 0);
});
