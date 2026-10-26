'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const telemetry = require('./helpers/telemetry-log-capture');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSpotActivation
} = require('../../packages/framework/dist/runtime/spots/spot-activation-state');
const {
  ZLinkSpotActorAdmissionCoordinator
} = require('../../packages/framework/dist/runtime/spots/spot-actor-admission-coordinator');
const {
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/runtime/spots/spot-serial-turn-executor');
const {
  ZLinkSpotActorHandlerRegistryRuntime
} = require('../../packages/framework/dist/runtime/actors/spot-actor-dispatch');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const follow = require('../../packages/framework/dist/runtime/actors/actor-message-follow-context');
const {
  ZLinkBufferMessage: Message
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  prepareZLinkRequestReply,
  identityRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-result');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const {
  ZLinkEntryActorRuntimeService
} = require('../../packages/framework/dist/runtime/host/entry-actor-runtime');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const {
  DefaultZLinkSessionActor
} = require('../../packages/framework/dist/runtime/streams/session-context');
const {
  ZLinkBoundActorRelaySender
} = require('../../packages/framework/dist/runtime/streams/bound-actor-relay-sender');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const {
  DefaultZLinkActorContext
} = require('../../packages/framework/dist/runtime/actors/actor-context');

function deferred() {
  let resolve;
  const promise = new Promise((done) => {
    resolve = done;
  });
  return { promise, resolve };
}
class Ping {
  constructor(value) {
    this.value = value;
  }
}
class Player {
  constructor() {
    this.context = { actorId: 'alice', meshName: 'mesh' };
  }
}
function fixture() {
  const calls = [];
  const reporter = new ZLinkDispatchErrorReporter(
    undefined,
    undefined,
    { reportRuntimeTaskException() {} },
    {
      diagnostics: { messageFlow: 'normal', sampleRate: 1, includeMessageSizes: false },
      liveMode: { mode: 'normal' }
    }
  );
  const actor = new Player();
  class Handler {
    async handle(spot, _actor, _context, packet) {
      calls.push([spot.context.spotId, packet.value]);
      return { value: packet.value };
    }
  }
  const registry = new ZLinkSpotActorHandlerRegistryRuntime();
  for (const kind of ['send', 'request'])
    registry.addPacket({ kind, packetName: 'Ping', actorType: Player, handlerType: Handler });
  function activation(id) {
    return new ZLinkSpotActivation({
      meshName: 'mesh',
      spotId: id,
      domain: {
        kind: 'user',
        executionMode: 'spot_wide',
        relocationCoordinationMode: 'framework_managed'
      },
      spotType: Object,
      spot: { context: { spotId: id, meshName: 'mesh' } },
      serial: new ZLinkSpotSerialTurnExecutor(),
      timers: {},
      actorHandlers: registry,
      handlers: {}
    });
  }
  const source = activation('source-spot');
  const target = activation('target-spot');
  source.commitActorJoin(actor);
  const targetAdmission = new ZLinkSpotActorAdmissionCoordinator({ dispatchErrors: reporter });
  async function deliver(payload) {
    const parts = [payload.header, payload.payload].map((value) =>
      Message.from(Buffer.from(value, 'base64'))
    );
    try {
      return await targetAdmission.dispatchActorPacket(
        'remote',
        target,
        'alice',
        parts,
        payload.returnResponse
      );
    } finally {
      parts.forEach((part) => part.close());
    }
  }
  const sourceOwner = {
    meshName: 'mesh',
    objectGeneration: 1n,
    ownerId: 'source-owner',
    ownerLeaseGeneration: 1n,
    nodeRid: 'source',
    nodeGeneration: 1n,
    authorityOwnerGeneration: 1n
  };
  const handoff = new framework.ZLinkActorHandoffCoordinator({
    requestSource: () => sourceOwner,
    validateReplySource: () => true,
    routedTransport: {
      sendToSpot: (_route, payload) => deliver(payload),
      requestToSpot: (_route, payload) =>
        prepareZLinkRequestReply(
          deliver(payload).then((response) => ({ ok: true, response })),
          identityRequestReply
        )
    }
  });
  const admission = new ZLinkSpotActorAdmissionCoordinator({
    actorHandoffRuntime: handoff,
    dispatchErrors: reporter
  });
  function move() {
    handoff.begin('alice', 1n);
    handoff.snapshot('alice');
    source.commitActorDeparture('alice');
    target.commitActorJoin(actor);
    handoff.complete(
      'alice',
      {
        routerChannelId: 'mesh',
        targetNodeRid: 'target',
        spotId: 'target-spot',
        spotKind: 'user',
        targetNodeGeneration: 1n,
        authorityOwnerGeneration: 2n,
        targetOwnerId: 'target-owner',
        ownerLeaseGeneration: 1n
      },
      { actorId: 'alice', objectGeneration: 1n, nodeRid: 'target', meshName: 'mesh' },
      [],
      follow.ownerFence({
        ownerId: 'target-owner',
        ownerLeaseGeneration: 1n,
        nodeRid: 'target',
        nodeGeneration: 1n,
        authorityOwnerGeneration: 2n
      })
    );
  }
  const submitted = deferred();
  let terminal;
  let sequence = 0n;
  function receive(parts, request, options) {
    const owned = parts.map((part) =>
      Message.from(Buffer.from(typeof part.data === 'function' ? part.data() : part))
    );
    terminal = admission
      .dispatchActorPacket(
        'remote',
        source,
        'alice',
        owned,
        request,
        undefined,
        undefined,
        undefined,
        {
          target: options.routeFence,
          sourceNodeRid: 'requester',
          originalOperation: { high: 1n, low: ++sequence },
          originalReplyRouteId: sequence
        }
      )
      .finally(() => owned.forEach((part) => part.close()));
    submitted.resolve();
    return { high: 1n, low: sequence };
  }
  const client = new framework.DefaultZLinkActorClient({
    nodeProvider: () => ({
      status: () => ({ routingId: 'source' }),
      sendToActor(_ref, parts, options) {
        receive(parts, false, options);
        return 0;
      },
      requestToActor(_ref, parts, options) {
        return receive(parts, true, options);
      }
    }),
    locationResolver: () => ({
      async resolveDirectActorRoute() {
        return {
          kind: 'ready',
          route: {
            meshName: 'mesh',
            actorId: 'alice',
            actorRef: { actorId: 'alice', objectGeneration: 1n, nodeRid: 'source' },
            ownerNodeRid: 'source',
            ownerNodeGeneration: 1n,
            spotKind: 'user',
            spotId: 'source-spot',
            spotGeneration: 1n,
            membershipEpoch: 1n,
            ownerId: 'source-owner',
            ownerLeaseGeneration: 1n,
            authorityOwnerGeneration: 1n,
            authorityStoreVersion: 'v1'
          }
        };
      },
      invalidateActorRoute() {}
    }),
    completionTableProvider: () => ({
      async submit(submit) {
        submit();
        const response = await terminal;
        return {
          terminalResult: 0,
          failureErrno: 0,
          parts: [
            Message.from(
              protocol.encodeStreamFrame(
                {
                  kind: protocol.ZLinkStreamMessageKind.Response,
                  codec: protocol.ZLinkStreamCodec.Json,
                  flags: 0,
                  name: 'Ping',
                  metadata: new Map()
                },
                Buffer.from(JSON.stringify(response))
              )
            )
          ]
        };
      }
    })
  });
  return {
    calls,
    source,
    target,
    admission,
    targetAdmission,
    actor,
    move,
    client,
    submitted: submitted.promise,
    terminal: () => terminal
  };
}

test('Actor packet dispatch is reused for messages in one activation', async () => {
  const f = fixture();
  let actorPacketDispatch;

  for (const value of ['first', 'second']) {
    const parts = [
      Message.from(
        protocol.encodeStreamHeader({
          kind: protocol.ZLinkStreamMessageKind.Send,
          codec: protocol.ZLinkStreamCodec.Json,
          flags: 0,
          name: 'Ping',
          metadata: new Map()
        })
      ),
      Message.from(JSON.stringify({ value }))
    ];
    try {
      await f.admission.dispatchActorPacket('remote', f.source, 'alice', parts);
    } finally {
      parts.forEach((part) => part.close());
    }

    assert.ok(f.source.actorPacketDispatch);
    if (actorPacketDispatch === undefined) actorPacketDispatch = f.source.actorPacketDispatch;
    else assert.strictEqual(f.source.actorPacketDispatch, actorPacketDispatch);
  }

  assert.deepEqual(f.calls.map((call) => call[1]), ['first', 'second']);
});

for (const updated of [false, true]) {
  test(`physical disconnect after relocation reaches the current Spot once (binding route updated=${updated})`, async () => {
    telemetry.reset();
    const f = fixture();
    const state = new ZLinkActorRuntimeState('alice');
    state.setNativeActorRef({ actorId: 'alice', nodeRid: 'target', generation: 1n });
    state.setJoinedSpot('target-spot', f.target.spot, 1n, 2n);
    state.installBoundSessionBinding({
      routerChannelId: 'mesh',
      targetNodeRid: 'session-owner',
      spotId: 'session-owner',
      sessionNodeRid: 'session-owner',
      sessionRid: 'session',
      sessionOwnerNodeGeneration: 3n,
      sessionOwnerId: 'session-owner',
      sessionOwnerLeaseGeneration: 1n,
      bindingGeneration: 7n
    });
    f.actor.context = new DefaultZLinkActorContext(state);
    const callbacks = [];
    f.source.spot.onDisconnectActor = async () => callbacks.push('source-spot');
    f.target.spot.onDisconnectActor = async () => callbacks.push('target-spot');
    f.move();
    const spotManager = {
      dispatchRoutedActorPacket: (
        origin,
        spotId,
        actorId,
        parts,
        response,
        bound,
        ref,
        terminal,
        followOrigin
      ) => {
        assert.equal(spotId, 'target-spot');
        return f.targetAdmission.dispatchActorPacket(
          origin,
          f.target,
          actorId,
          parts,
          response,
          bound,
          ref,
          terminal,
          followOrigin
        );
      }
    };
    const runtime = new ZLinkEntryActorRuntimeService({
      actorManager: () => ({ getState: () => state }),
      spotManager: () => spotManager,

      spotNodeRuntime: () => undefined,
      streamBindingRuntime: {},
      boundSessionRelay: {}
    });
    const receiver = new ZLinkActorPacketRelay({
      meshRouters: {},
      actorManager: () => (updated ? { getState: () => state } : undefined),
      spotManager: () => spotManager,
      spotNodeRuntime: () => ({
        dispatchEntryActorPacket: async (_origin, actorId, parts, response, bound, ref) => {
          if (!updated)
            return f.admission.dispatchActorPacket(
              'remote',
              f.source,
              actorId,
              parts,
              response,
              bound,
              ref
            );
          const result = await runtime.routePacket(actorId, parts, response, bound, ref);
          assert.equal(result.handled, true);
          return result.response;
        }
      })
    });
    const senderRelay = new ZLinkActorPacketRelay({
      meshRouters: {},
      actorManager: () => undefined,
      spotNodeRuntime: () => ({
        primaryMeshNode: { status: () => ({ routingId: 'session-owner' }) }
      }),
      routeTransport: {
        request: (_channel, _node, _name, payload) =>
          prepareZLinkRequestReply(
            receiver.receiveRemoteActorPacketRelay(payload, {
              meshName: 'mesh',
              sourceNodeRid: 'session-owner'
            }),
            identityRequestReply
          )
      }
    });
    const binding = {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 3n,
      bindingGeneration: 7n
    };
    const stream = new ZLinkManagedStream({ async unbindActor() {} }, 'session');
    stream.actorBinding = () => binding;
    const actor = new DefaultZLinkSessionActor(
      {},
      {
        actorId: 'alice',
        objectGeneration: 1n,
        nodeRid: updated ? 'target' : 'source',
        meshName: 'mesh'
      },
      'binding'
    );
    const context = { boundActors: [actor], stream, bindLocal() {}, unbindLocal() {} };
    const registry = new ZLinkActorSessionBindingRegistry();
    await registry.bind(context, actor, actor.bindingToken, {
      ownerId: updated ? 'target-owner' : 'source-owner',
      ownerNodeGeneration: 1n,
      ownerLeaseGeneration: 1n,
      authorityOwnerGeneration: updated ? 2n : 1n
    });
    const sender = new ZLinkBoundActorRelaySender(
      registry,
      {},
      {
        notifyDisconnected: (selected, signal, snapshot, authority) =>
          senderRelay.notifyBoundActorDisconnected(selected, signal, snapshot, authority)
      }
    );
    await sender.notifyPhysicalDisconnect(context);
    await sender.notifyPhysicalDisconnect(context);
    assert.deepEqual(callbacks, ['target-spot']);
    assert.equal(
      telemetry.records.filter((record) => record.eventId === 'zlink.dispatch_error').length,
      0
    );
    assert.equal(
      telemetry.records.filter(
        (record) =>
          record.phase === 'received' &&
          record.spotId === 'target-spot' &&
          record.packetName === 'zlink.framework.actor.session_disconnected'
      ).length,
      1
    );
  });
}
for (const request of [false, true]) {
  test(
    `Actor ${request ? 'request' : 'send'} follows ownership committed while waiting for its mailbox`,
    { timeout: 3000 },
    async () => {
      telemetry.reset();
      const f = fixture();
      const entered = deferred();
      const release = deferred();
      const occupied = f.source.executeActor('alice', (serial) =>
        serial.execute(async () => {
          entered.resolve();
          await release.promise;
        })
      );
      await entered.promise;
      const call = (
        request
          ? f.client.requestToActor('alice', new Ping('boundary'))
          : f.client.sendToActor('alice', new Ping('boundary'))
      ).submit();
      const completion = call.then(
        (value) => ({ value }),
        (error) => ({ error })
      );
      await Promise.race([
        f.submitted,
        completion.then((result) => {
          if (result.error) throw result.error;
        })
      ]);
      f.move();
      release.resolve();
      try {
        await occupied;
        const result = await completion;
        if (!request) await f.terminal();
        if (result.error) throw result.error;
        if (request) assert.deepEqual(result.value, { value: 'boundary' });
        assert.deepEqual(f.calls, [['target-spot', 'boundary']]);
        assert.equal(
          telemetry.records.filter((record) => record.reason === 'no_handler').length,
          0
        );
        assert.equal(
          telemetry.records.filter(
            (record) =>
              record.phase === (request ? 'replied' : 'completed') &&
              record.spotId === 'target-spot'
          ).length,
          1
        );
      } finally {
        release.resolve();
      }
    }
  );
}
for (const present of [false, true]) {
  test(`Actor ${present ? 'handler absence is no_handler' : 'target absence is stale_target'}`, async () => {
    telemetry.reset();
    const f = fixture();
    if (!present) f.source.commitActorDeparture('alice');
    const parts = [
      Message.from(
        protocol.encodeStreamHeader({
          kind: protocol.ZLinkStreamMessageKind.Send,
          codec: protocol.ZLinkStreamCodec.Json,
          flags: 0,
          name: 'Unregistered',
          metadata: new Map()
        })
      ),
      Message.from('{}')
    ];
    try {
      const pending = f.admission.dispatchActorPacket('remote', f.source, 'alice', parts);
      if (present) await assert.rejects(pending);
      else await pending;
      const failures = telemetry.records.filter(
        (record) => record.eventId === 'zlink.dispatch_error'
      );
      assert.equal(failures.length, 1);
      assert.equal(failures[0].reason, present ? 'no_handler' : 'stale_target');
    } finally {
      parts.forEach((part) => part.close());
    }
  });
}

for (const returnResponse of [false, true]) {
  test(`prewarm request retains its existing reply capability (returnResponse=${returnResponse})`, async () => {
    const {
      ZLinkSpotActorPacketDispatch
    } = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-dispatch');
    let arrival;
    let consultations = 0;
    const dispatch = new ZLinkSpotActorPacketDispatch({
      spot: {},
      spotId: () => 'target-spot',
      registry: {},
      resolveActor: () => undefined,
      onDisconnectActor: async () => {},
      routeToActorJoinPrewarm: (_actorId, _generation, accepted) => {
        consultations++;
        arrival = accepted;
        return 'parked';
      }
    });
    const parts = [
      Message.from(
        protocol.encodeStreamHeader({
          kind: protocol.ZLinkStreamMessageKind.Request,
          codec: protocol.ZLinkStreamCodec.Json,
          flags: 0,
          name: 'Ask',
          requestSeq: 1n,
          metadata: new Map()
        })
      ),
      Message.from('{}')
    ];
    try {
      const response = dispatch.dispatch(undefined, 'remote', 'alice', parts, returnResponse, undefined, {
        actorId: 'alice',
        objectGeneration: 1n,
        nodeRid: 'target',
        meshName: 'mesh'
      });
      if (!returnResponse) {
        await assert.rejects(
          response,
          (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
        );
        assert.equal(consultations, 0);
      } else {
        assert.equal(consultations, 1);
        assert.equal(arrival.returnResponse, true);
        assert.equal(arrival.requestTerminal, undefined);
        arrival.resolve({ answer: 7 });
        assert.deepEqual(await response, { answer: 7 });
      }
    } finally {
      parts.forEach((part) => part.close());
    }
  });
}
