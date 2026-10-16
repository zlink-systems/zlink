const assert = require('node:assert/strict');
const test = require('node:test');
const telemetry = require('./helpers/telemetry-log-capture');
const {
  ZLinkSpotActorPacketDispatch
} = require('../../packages/framework/dist/runtime/spots/spot-actor-packet-dispatch');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist/contracts');

test('malformed disconnect is dropped with ProtocolError diagnostics', async () => {
  let callbacks = 0;
  const errors = [];
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {},
    spotId: () => 'spot',
    registry: {},
    resolveActor: () => ({ context: { actorId: 'actor' } }),
    onDisconnectActor: async () => {
      callbacks++;
    },
    dispatchErrors: {
      flow: { flowCreationEnabled: () => false, enabled: () => false, begin: () => undefined },
      captureEnabled: () => true,
      report: (event) => errors.push(event)
    }
  });
  const header = protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Send,
    codec: protocol.ZLinkStreamCodec.Raw,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    name: 'zlink.framework.actor.session_disconnected',
    metadata: new Map()
  });
  await dispatch.dispatch('remote', 'actor', [
    new ZLinkBufferMessage(header),
    new ZLinkBufferMessage(Buffer.alloc(0))
  ]);
  assert.equal(callbacks, 0);
  assert.equal(errors.length, 1);
  assert.equal(errors[0].error.kind, ZLinkFrameworkErrorKind.ProtocolError);
  assert.equal(errors[0].severity, 'warning');
});

const fs = require('node:fs');
const path = require('node:path');
const wire = require('../../packages/framework/dist/runtime/actors/session-disconnected-wire');
const {
  decodeRoutingId,
  encodeRoutingIdStorageHex
} = require('../../packages/framework/dist/runtime/routing-id');
const {
  DefaultZLinkActorContext
} = require('../../packages/framework/dist/runtime/actors/actor-context');
const {
  ZLinkActorRuntimeState
} = require('../../packages/framework/dist/runtime/actors/actor-runtime-state');
const fixtures = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/protocol/golden/session-disconnected-v1.json'),
    'utf8'
  )
);

for (const vector of fixtures.canonical) {
  test(`disconnect shared golden ${vector.name}`, () => {
    const rid = decodeRoutingId(vector.sessionOwnerNodeRidHex, vector.sessionOwnerNodeRidHex);
    const bytes = wire.encodeSessionDisconnected(
      rid,
      BigInt(vector.sessionOwnerNodeGeneration),
      BigInt(vector.bindingGeneration)
    );
    assert.equal(bytes.toString('hex'), vector.hex);
    const decoded = wire.decodeSessionDisconnected(Buffer.from(vector.hex, 'hex'));
    assert.equal(
      encodeRoutingIdStorageHex(decoded.sessionOwnerNodeRid),
      vector.sessionOwnerNodeRidHex
    );
    assert.equal(decoded.sessionOwnerNodeGeneration.toString(), vector.sessionOwnerNodeGeneration);
    assert.equal(decoded.bindingGeneration.toString(), vector.bindingGeneration);
  });
}
for (const vector of fixtures.rejected) {
  test(`disconnect rejects ${vector.name}`, () => {
    assert.equal(wire.decodeSessionDisconnected(Buffer.from(vector.hex, 'hex')), undefined);
  });
}

for (const [name, node, ownerGeneration, bindingGeneration, expected] of [
  ['same binding through Message Follow node', 'node-a', 2n, 7n, 1],
  ['other Session owner RID', 'node-b', 2n, 7n, 0],
  ['other Session owner lifecycle', 'node-a', 3n, 7n, 0],
  ['other binding generation', 'node-a', 2n, 8n, 0],
  ['no current binding', undefined, undefined, undefined, 1]
]) {
  test(`Actor turn disconnect: ${name}`, async () => {
    const state = new ZLinkActorRuntimeState('actor');
    if (node !== undefined)
      state.installBoundSessionBinding({
        routerChannelId: 'route',
        targetNodeRid: node,
        spotId: 'entry',
        sessionNodeRid: node,
        sessionRid: 'session',
        sessionOwnerNodeGeneration: ownerGeneration,
        sessionOwnerId: 'owner',
        sessionOwnerLeaseGeneration: 1n,
        bindingGeneration
      });
    const actor = { context: new DefaultZLinkActorContext(state) };
    let callbacks = 0;
    const errors = [];
    const dispatch = new ZLinkSpotActorPacketDispatch({
      spot: {},
      spotId: () => 'spot',
      registry: {},
      resolveActor: () => actor,
      onDisconnectActor: async () => {
        callbacks++;
      },
      dispatchErrors: {
        flow: { flowCreationEnabled: () => false, enabled: () => false, begin: () => undefined },
        report: (e) => errors.push(e)
      }
    });
    const header = protocol.encodeStreamHeader({
      kind: protocol.ZLinkStreamMessageKind.Send,
      codec: protocol.ZLinkStreamCodec.Raw,
      flags: protocol.ZLinkStreamHeaderFlags.None,
      name: 'zlink.framework.actor.session_disconnected',
      metadata: new Map()
    });
    // The delivery's transport coordinates deliberately name a forwarding node.
    await dispatch.dispatchDelivery({
      origin: 'remote',
      actorId: 'actor',
      sourceSessionRid: 'follow-hop-session',
      remoteBoundSessionTarget: { sessionNodeRid: 'follow-hop-node' },
      parts: [
        new ZLinkBufferMessage(header),
        new ZLinkBufferMessage(Buffer.from(fixtures.canonical[0].hex, 'hex'))
      ]
    });
    assert.equal(callbacks, expected);
    assert.equal(errors.length, 0);
  });
}

const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const {
  ZLinkNativeFallbackBoundSession
} = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const { SeverityNumber } = require('@opentelemetry/api-logs');
const {
  ZLinkActorSerialExecutor
} = require('../../packages/framework/dist/runtime/actors/actor-mailbox');
const {
  currentZLinkActorExecution
} = require('../../packages/framework/dist/runtime/actors/actor-execution-context');

test('host relay compares the body inside the Actor turn and awaits disconnect terminal', async () => {
  const state = new ZLinkActorRuntimeState('actor');
  state.installBoundSessionBinding({
    routerChannelId: 'route',
    targetNodeRid: 'node-a',
    spotId: 'entry',
    sessionNodeRid: 'node-a',
    sessionRid: 'session',
    sessionOwnerNodeGeneration: 2n,
    sessionOwnerId: 'owner',
    sessionOwnerLeaseGeneration: 1n,
    bindingGeneration: 7n
  });
  const actor = { context: new DefaultZLinkActorContext(state) };
  const mailbox = new ZLinkActorSerialExecutor('actor', 'entry');
  let callbacks = 0;
  let release;
  const terminal = new Promise((resolve) => {
    release = resolve;
  });
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {},
    spotId: () => 'entry',
    registry: {},
    resolveActor: () => actor,
    onDisconnectActor: async () => {
      assert.equal(currentZLinkActorExecution().actorId, 'actor');
      callbacks++;
      await terminal;
    }
  });
  const relay = new ZLinkActorPacketRelay({
    meshRouters: {},
    actorManager: () => ({ getState: () => state }),
    spotNodeRuntime: () => ({
      dispatchEntryActorPacket: (...args) => mailbox.execute(() => dispatch.dispatch(...args))
    })
  });
  const header = protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Send,
    codec: protocol.ZLinkStreamCodec.Raw,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    name: 'zlink.framework.actor.session_disconnected',
    metadata: new Map()
  });
  let completed = false;
  const received = relay
    .receiveRemoteActorPacketRelay(
      {
        packetName: '__zlink.actor.packet.relay',
        actorId: 'actor',
        routerChannelId: 'route',
        boundSessionTargetNodeRid: 'follow-node',
        boundSessionSpotId: 'follow-entry',
        header: Buffer.from(header).toString('base64'),
        payload: Buffer.from(fixtures.canonical[0].hex, 'hex').toString('base64')
      },
      { meshName: 'actors', sourceNodeRid: 'follow-node' }
    )
    .then((result) => {
      completed = true;
      return result;
    });
  await new Promise((resolve) => setImmediate(resolve));
  try {
    assert.equal(callbacks, 1);
    assert.equal(completed, false);
  } finally {
    release();
  }
  assert.equal((await received).ok, true);
  await mailbox.close();
});

const {
  ZLinkBoundActorRelaySender
} = require('../../packages/framework/dist/runtime/streams/bound-actor-relay-sender');
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');
for (const physical of [false, true]) {
  test(`disconnect captures Session owner identity before cleanup (physical=${physical})`, async () => {
    const actor = { actorId: 'actor', bindingToken: 'token' };
    const binding = {
      sessionOwnerNodeRid: 'node-a',
      sessionOwnerNodeGeneration: 2n,
      bindingGeneration: 7n
    };
    const stream = Object.create(ZLinkManagedStream.prototype);
    let current = binding;
    stream.actorBinding = () => current;
    stream.unbindActor = async () => {
      current = undefined;
    };
    const context = { stream, boundActors: [actor] };
    const route = { actor, context, bindingToken: 'token' };
    let captured;
    const sender = new ZLinkBoundActorRelaySender(
      {
        requireCurrentToken: async () => {},
        requireRoute: async () => route,
        route: async () => route,
        unbind: async () => {
          current = undefined;
        }
      },
      undefined,
      {
        notifyDisconnected: async (_actor, _signal, snapshot) => {
          captured = snapshot;
        }
      }
    );
    if (physical) await sender.notifyPhysicalDisconnect(context);
    else await sender.notifyDisconnected(actor);
    assert.equal(current, undefined);
    assert.equal(captured, binding);
  });
}

test('local Session disconnect uses the same Actor-turn identity comparison', async () => {
  const state = new ZLinkActorRuntimeState('actor');
  state.installBoundSessionBinding({
    routerChannelId: 'route',
    targetNodeRid: 'node-a',
    spotId: 'entry',
    sessionNodeRid: 'node-a',
    sessionRid: 'session',
    sessionOwnerNodeGeneration: 2n,
    sessionOwnerId: 'owner',
    sessionOwnerLeaseGeneration: 1n,
    bindingGeneration: 8n
  });
  const mailbox = new ZLinkActorSerialExecutor('actor', 'entry');
  const actor = { context: new DefaultZLinkActorContext(state) };
  let callbacks = 0;
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {},
    spotId: () => 'entry',
    registry: {},
    resolveActor: () => actor,
    onDisconnectActor: async () => {
      callbacks++;
    }
  });
  const relay = new ZLinkActorPacketRelay({
    meshRouters: {},
    actorManager: () => ({ getState: () => state }),
    spotNodeRuntime: () => ({
      dispatchEntryActorPacket: (...args) => mailbox.execute(() => dispatch.dispatch(...args))
    })
  });
  await relay.notifyLocalActorDisconnectedById('actor', undefined, {
    sessionOwnerNodeRid: 'node-a',
    sessionOwnerNodeGeneration: 2n,
    bindingGeneration: 7n
  });
  assert.equal(callbacks, 0);
  await mailbox.close();
});

test('both disconnect senders use the shared body and preserve a captured binding through rebind', async () => {
  const sent = [];
  const target = {
    routerChannelId: 'route',
    targetNodeRid: 'actor-node',
    spotId: 'entry',
    sessionNodeRid: 'node-a',
    sessionOwnerNodeGeneration: 2n,
    bindingGeneration: 7n
  };
  const relay = new ZLinkActorPacketRelay({
    meshRouters: {},
    actorManager: () => ({
      getState: () => ({ boundSession: { ...target, bindingGeneration: 8n } })
    }),
    routeTransport: {
      request: async (...args) => {
        sent.push(args[3]);
      }
    }
  });
  await relay.notifyRemoteActorDisconnected('actor', target, undefined, undefined, {
    sessionOwnerNodeRid: 'node-a',
    sessionOwnerNodeGeneration: 2n,
    bindingGeneration: 7n
  });
  assert.equal(Buffer.from(sent[0].payload, 'base64').toString('hex'), fixtures.canonical[0].hex);
  const fallback = new ZLinkNativeFallbackBoundSession({
    actorId: 'actor',
    remoteBoundSessionTargetProvider: () => target,
    routedTransport: {
      sendToSpot: async (_address, payload) => {
        sent.push(payload);
      }
    }
  });
  await fallback.disconnect();
  assert.equal(Buffer.from(sent[1].payload, 'base64').toString('hex'), fixtures.canonical[0].hex);
});

test('malformed disconnect records a ProtocolError Warning at Errors level', async () => {
  telemetry.reset();
  let callbacks = 0;
  const reporter = new ZLinkDispatchErrorReporter(undefined, undefined, {
    reportUnhandledCallbackException(error) {
      throw error;
    }
  });
  const dispatch = new ZLinkSpotActorPacketDispatch({
    spot: {},
    spotId: () => 'spot',
    registry: {},
    resolveActor: () => ({ context: { actorId: 'actor' } }),
    onDisconnectActor: async () => {
      callbacks++;
    },
    dispatchErrors: reporter
  });
  const header = protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Send,
    codec: protocol.ZLinkStreamCodec.Raw,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    name: 'zlink.framework.actor.session_disconnected',
    metadata: new Map()
  });
  for (const vector of fixtures.rejected) {
    await dispatch.dispatch('remote', 'actor', [
      new ZLinkBufferMessage(header),
      new ZLinkBufferMessage(Buffer.from(vector.hex, 'hex'))
    ]);
  }
  assert.equal(callbacks, 0);
  const errors = telemetry.records.filter((r) => r.eventId === 'zlink.dispatch_error');
  assert.equal(errors.length, fixtures.rejected.length);
  for (const error of errors) {
    assert.equal(error.severityNumber, SeverityNumber.WARN);
    assert.match(error.errorMessage, /ProtocolError/);
  }
});
