const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const {
  RequestResult,
  SubmitResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  createSessionDispatchContext
} = require('../../packages/framework/dist/runtime/streams/session-context');
const {
  ZLinkNativeFallbackBoundSession
} = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkRemoteBoundSessionRelay
} = require('../../packages/framework/dist/runtime/host/remote-bound-session-relay');
const {
  ZLinkBoundSessionRelay
} = require('../../packages/framework/dist/runtime/host/bound-session-relay');
const {
  ZLinkRemoteActorPacketTargetStore
} = require('../../packages/framework/dist/runtime/host/remote-actor-packet-target-store');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const {
  ZLinkActorSessionLifecycleCoordinator
} = require('../../packages/framework/dist/runtime/streams/actor-session-lifecycle-coordinator');
const {
  ZLinkBoundActorRelaySender
} = require('../../packages/framework/dist/runtime/streams/bound-actor-relay-sender');
const {
  actorSessionBindingRuntimeOwner,
  registerActorSessionBindingRuntimeOwner
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-runtime-owner');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const serviceStatefulWire = require(
  '../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec'
);
const serviceWire = require(
  '../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec'
);
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const actorPacketWire = require('../../packages/framework/dist/runtime/actors/actor-packet-relay-wire');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ServiceWireProtocolError
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const zlink = require('@zlink-systems/zlink');

test('stream runtime is exported from framework root surface', () => {
  assert.equal(typeof framework.ZLinkStreamBindingRuntime, 'function');
  assert.equal(typeof framework.DefaultZLinkSessionContext, 'function');
});

test('clearing an Actor packet target preserves Session relocation terminal ownership', () => {
  const relay = new ZLinkBoundSessionRelay({
    routeTransport: {},
    streamBindingRuntime: () => ({}),
    meshRouters: {},
    actorManager: () => undefined,
    spotManager: () => undefined,
    spotNodeRuntime: () => undefined,
    detachedTaskRunner: { runDetached() {} },
    actorSessionNode: () => undefined,
    authorityStore: () => undefined,
    destroyedActorRefs: new Map(),
    errorSink: () => ({ reportRuntimeTaskException() {} }),
    boundSessionFactory() {
      throw new Error('not used');
    }
  });
  let packetTargetClears = 0;
  let ownershipClears = 0;
  relay.actorPackets.clearRemoteActorPacketTarget = () => { packetTargetClears++; };
  relay.boundSessions.clearOwnership = () => { ownershipClears++; };

  relay.clearRemoteActorPacketTarget('actor-relocating');

  assert.equal(packetTargetClears, 1);
  assert.equal(ownershipClears, 0);
});

test('managed stream binds Session Actors through the Framework service without binding service APIs', async () => {
  const actor = {
    actorId: 'actor-framework-service',
    generation: 7n,
    nodeRid: 'node-play'
  };
  const operations = new Map();
  const bindings = [];
  let bindAttempts = 0;
  let nextOperation = 1n;
  const operation = (kind) => {
    const id = { high: 0n, low: nextOperation++ };
    operations.set(id.low, kind);
    return id;
  };
  const service = {
    start() {},
    shutdown() { return 0; },
    close() {},
    status() {
      return {
        state: 2,
        lifecycleGeneration: 1n,
        sessionCount: 1n,
        bindingCount: BigInt(bindings.length),
        pendingMessageCount: 0n,
        pendingByteCount: 0n,
        lastError: 0
      };
    },
    lookupActor() { return operation('lookup'); },
    bindActor(sessionRid, value) {
      bindAttempts += 1;
      if (bindAttempts === 1) {
        throw Object.assign(new Error('diagnostic text is not part of classification'), {
          result: SubmitResult.NotConnected
        });
      }
      bindings.push({
        sessionRid,
        actor: value,
        bindingGeneration: 1n,
        membershipEpoch: 1n
      });
      return operation('bind');
    },
    unbindActor() { return operation('unbind'); },
    bindings() { return bindings; },
    sendToActor() { return 0; }
  };
  const completions = {
    submit(operation) {
      const id = operation();
      const kind = operations.get(id.low);
      return Promise.resolve({
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? { kind: 'actorLookupCompletion', location: { actor } }
          : null,
        parts: []
      });
    }
  };
  const rawStreamSocket = {
    sendTimeoutMs: 1000,
    sendHighWaterMark: 16,
    onSendReady() {},
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; }
  };
  const stream = new framework.ZLinkManagedStream(
    rawStreamSocket,
    'session-rid',
    undefined,
    service,
    completions
  );

  await stream.bindActor({
    actorId: actor.actorId,
    objectGeneration: actor.generation,
    meshName: 'play',
    nodeRid: actor.nodeRid
  }, 1000);

  assert.equal(bindings.length, 1);
  assert.equal(bindAttempts, 2);
  assert.equal(typeof rawStreamSocket.bindActor, 'undefined');
  assert.equal(typeof rawStreamSocket.unbindActor, 'undefined');
  assert.equal(typeof rawStreamSocket.sendBoundActor, 'undefined');
});

test('managed stream bind admission deadline preserves the route failure as a typed DeadlineExceeded', async () => {
  const routeFailure = Object.assign(new Error('remote route is not connected'), {
    result: SubmitResult.NotConnected
  });
  const service = {
    start() {}, shutdown() { return 0; }, close() {},
    status() {
      return {
        state: 2, lifecycleGeneration: 1n, sessionCount: 4n,
        bindingCount: 0n, pendingMessageCount: 0n, pendingByteCount: 0n, lastError: 0
      };
    },
    lookupActor() { return { low: 1n }; },
    bindActor() { throw routeFailure; },
    unbindActor() { throw new Error('not used'); },
    bindings() { return []; }, sendToActor() { return 0; }
  };
  const stream = new framework.ZLinkManagedStream(
    { send() { return true; }, disconnectPeer() {}, recv() { return undefined; } },
    'session-rid',
    undefined,
    service,
    {
      submit(operation) {
        const result = operation();
        return Promise.resolve({
          terminalResult: 0,
          failureErrno: 0,
          operationKind: 0,
          kindData: result?.low === 1n
            ? {
                kind: 'actorLookupCompletion',
                location: { actor: { actorId: 'actor-deadline', generation: 1n, nodeRid: 'node-a' } }
              }
            : null,
          parts: []
        });
      }
    }
  );

  await assert.rejects(
    stream.bindActor({
      actorId: 'actor-deadline', objectGeneration: 1n, meshName: 'play', nodeRid: 'node-a'
    }, 0),
    (error) => {
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(
        framework.internalFrameworkErrorKind(error),
        framework.ZLinkFrameworkInternalErrorKind.DeadlineExceeded
      );
      assert.equal(error.cause, routeFailure);
      assert.match(error.message, /actor bind deadline/);
      return true;
    }
  );
});

test('session actor dispatch relay preserves the supplied request correlation', async () => {
  const relayed = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    async relay(_actor, header) {
      relayed.push(header);
      return true;
    }
  });
  const context = runtime.createSessionContext(fakeStream('dispatch-relay', 'dispatch-relay-rid'));
  const actor = await context.actors.bind({
    nodeRid: 'node-a', actorId: 'actor-dispatch-relay', generation: 1n
  });
  const header = {
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'CorrelatedRelay',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    requestSeq: 81n
  };
  context.enterDispatch(header);
  try {
    await actor.relay(
      createSessionDispatchContext(header),
      framework.ZLinkMessage.fromEncoded(
        framework.ZLinkEncodedPayload.from(Buffer.from('{"relay":true}'))
      )
    );
  } finally {
    context.exitDispatch();
  }
  assert.equal(relayed.length, 1);
  assert.equal(relayed[0].requestSeq, 81n);
  assert.equal(relayed[0].name, 'CorrelatedRelay');
});

test('managed stream treats an actor-destroy stale unbind as idempotent cleanup', async () => {
  const operations = new Map();
  let nextOperation = 1n;
  const operation = (kind) => {
    const id = { high: 0n, low: nextOperation++ };
    operations.set(id.low, kind);
    return id;
  };
  const service = {
    start() {},
    shutdown() { return 0; },
    close() {},
    status() {
      return {
        state: 2,
        lifecycleGeneration: 1n,
        sessionCount: 1n,
        bindingCount: 0n,
        pendingMessageCount: 0n,
        pendingByteCount: 0n,
        lastError: 0
      };
    },
    lookupActor() { return operation('lookup'); },
    bindActor() { return operation('bind'); },
    unbindActor() { return operation('unbind'); },
    bindings() { return [{ actor: { actorId: 'actor-destroy', generation: 1n }, bindingGeneration: 1n }]; },
    sendToActor() { return 0; }
  };
  const completions = {
    async submit(operation) {
      const id = operation();
      const kind = operations.get(id.low);
      return {
        terminalResult: kind === 'unbind' ? RequestResult.NotFound : RequestResult.Ok,
        failureErrno: kind === 'unbind' ? 21 : 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? {
              kind: 'actorLookupCompletion',
              location: { actor: { actorId: 'actor-destroy', generation: 1n, nodeRid: 'node-a' } }
            }
          : null,
        parts: []
      };
    }
  };
  const stream = new framework.ZLinkManagedStream(
    {
      sendTimeoutMs: 1000,
      sendHighWaterMark: 16,
      onSendReady() {},
      send() { return true; },
      disconnectPeer() {},
      recv() { return undefined; }
    },
    'session-rid',
    undefined,
    service,
    completions
  );

  await stream.bindActor({
    actorId: 'actor-destroy',
    objectGeneration: 1n,
    meshName: 'play',
    nodeRid: 'node-a'
  }, 1000);
  await assert.doesNotReject(() => stream.unbindActor('actor-destroy', 1000));
});

test('managed stream delegates each call timeout to binding-owned admission', async () => {
  const observed = [];
  const socket = {
    sendTimeoutMs: 10,
    sendHighWaterMark: 16,
    onSendReady() {},
    send() { return true; },
    async sendAsync(_routingId, _payload, timeoutMs) {
      observed.push(timeoutMs);
    },
    disconnectPeer() {},
    recv() { return undefined; }
  };
  const stream = new framework.ZLinkManagedStream(
    socket,
    'session-timeout-bound'
  );
  const message = zlink.Message.from('payload');
  try {
    assert.deepEqual(await stream.submitRaw(message, undefined, 25), {
      status: ZLinkSubmitStatus.Submitted
    });
    assert.deepEqual(await stream.submitRaw(message, undefined, 4), {
      status: ZLinkSubmitStatus.Submitted
    });
  } finally {
    message.close();
  }
  assert.deepEqual(observed, [25, 4]);
});

test('managed stream skips native unbind after transport teardown', async () => {
  const operations = new Map();
  let nextOperation = 1n;
  let nativeUnbinds = 0;
  const operation = (kind) => {
    const id = { high: 0n, low: nextOperation++ };
    operations.set(id.low, kind);
    return id;
  };
  const binding = {
    actor: { actorId: 'actor-closed', generation: 1n },
    bindingGeneration: 1n
  };
  const service = {
    start() {},
    shutdown() { return 0; },
    close() {},
    status() {
      return {
        state: 2,
        lifecycleGeneration: 1n,
        sessionCount: 1n,
        bindingCount: 1n,
        pendingMessageCount: 0n,
        pendingByteCount: 0n,
        lastError: 0
      };
    },
    lookupActor() { return operation('lookup'); },
    bindActor() { return operation('bind'); },
    unbindActor() {
      nativeUnbinds += 1;
      return operation('unbind');
    },
    bindings() { return [binding]; },
    sendToActor() { return 0; }
  };
  const completions = {
    async submit(operation) {
      const id = operation();
      const kind = operations.get(id.low);
      return {
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? {
              kind: 'actorLookupCompletion',
              location: {
                actor: {
                  actorId: 'actor-closed',
                  generation: 1n,
                  nodeRid: 'node-a'
                }
              }
            }
          : null,
        parts: []
      };
    }
  };
  const stream = new framework.ZLinkManagedStream(
    {
      sendTimeoutMs: 1000,
      sendHighWaterMark: 16,
      onSendReady() {},
      send() { return true; },
      disconnectPeer() {},
      recv() { return undefined; }
    },
    'session-rid',
    undefined,
    service,
    completions
  );

  await stream.bindActor({
    actorId: 'actor-closed',
    objectGeneration: 1n,
    meshName: 'play',
    nodeRid: 'node-a'
  }, 1000);
  await stream.close();
  await stream.unbindActor('actor-closed', 1000);

  assert.equal(nativeUnbinds, 0);
});

test('managed stream fences a native bind that completes after transport teardown', async () => {
  const operations = new Map();
  let nextOperation = 1n;
  let releaseBind;
  const bindCanFinish = new Promise((resolve) => { releaseBind = resolve; });
  let nativeUnbinds = 0;
  const operation = (kind) => {
    const id = { high: 0n, low: nextOperation++ };
    operations.set(id.low, kind);
    return id;
  };
  const binding = {
    actor: { actorId: 'actor-bind-closed', generation: 1n },
    bindingGeneration: 9n
  };
  const service = {
    start() {}, shutdown() { return 0; }, close() {},
    status() {
      return {
        state: 2, lifecycleGeneration: 1n, sessionCount: 1n,
        bindingCount: 1n, pendingMessageCount: 0n, pendingByteCount: 0n, lastError: 0
      };
    },
    lookupActor() { return operation('lookup'); },
    bindActor() { return operation('bind'); },
    unbindActor(_sessionRid, _actor, bindingGeneration) {
      nativeUnbinds += 1;
      assert.equal(bindingGeneration, 9n);
      return operation('unbind');
    },
    bindings() { return [binding]; },
    sendToActor() { throw new Error('closed binding must not send'); }
  };
  const completions = {
    async submit(work) {
      const id = work();
      const kind = operations.get(id.low);
      if (kind === 'bind') await bindCanFinish;
      return {
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? { kind: 'actorLookupCompletion', location: { actor: {
            actorId: 'actor-bind-closed', generation: 1n, nodeRid: 'node-a'
          } } }
          : null,
        parts: []
      };
    }
  };
  const stream = new framework.ZLinkManagedStream(
    { send() { return true; }, disconnectPeer() {}, recv() { return undefined; } },
    'session-rid', undefined, service, completions
  );

  const bindingAttempt = stream.bindActor({
    actorId: 'actor-bind-closed', objectGeneration: 1n, meshName: 'play', nodeRid: 'node-a'
  }, 1000);
  await new Promise((resolve) => setImmediate(resolve));
  stream.markTransportClosed();
  releaseBind();

  await assert.rejects(bindingAttempt, (error) => {
    assert.equal(
      framework.internalFrameworkErrorKind(error),
      framework.ZLinkFrameworkInternalErrorKind.RouteNotConnected
    );
    return true;
  });
  assert.equal(nativeUnbinds, 1);
  await assert.rejects(() => stream.sendBoundActor('actor-bind-closed', []));
});

test('managed stream accepts an internal unbind result after the exact delivery is gone', async () => {
  const operations = new Map();
  let nextOperation = 1n;
  let bound = true;
  const operation = (kind) => {
    const id = { high: 0n, low: nextOperation++ };
    operations.set(id.low, kind);
    return id;
  };
  const service = {
    start() {},
    shutdown() { return 0; },
    close() {},
    status() {
      return {
        state: 2,
        lifecycleGeneration: 1n,
        sessionCount: 1n,
        bindingCount: bound ? 1n : 0n,
        pendingMessageCount: 0n,
        pendingByteCount: 0n,
        lastError: 0
      };
    },
    lookupActor() { return operation('lookup'); },
    bindActor() { return operation('bind'); },
    unbindActor() {
      bound = false;
      return operation('unbind');
    },
    bindings() {
      return bound
        ? [{
            sessionRid: 'session-rid',
            actor: { actorId: 'actor-stale', generation: 1n },
            bindingGeneration: 1n,
            membershipEpoch: 1n
          }]
        : [];
    },
    sendToActor() { return 0; }
  };
  const completions = {
    async submit(operation) {
      const id = operation();
      const kind = operations.get(id.low);
      return {
        terminalResult: kind === 'unbind' ? RequestResult.InternalError : 0,
        failureErrno: kind === 'unbind' ? 17 : 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? {
              kind: 'actorLookupCompletion',
              location: {
                actor: {
                  actorId: 'actor-stale',
                  generation: 1n,
                  nodeRid: 'node-a'
                }
              }
            }
          : null,
        parts: []
      };
    }
  };
  const stream = new framework.ZLinkManagedStream(
    {
      sendTimeoutMs: 1000,
      sendHighWaterMark: 16,
      onSendReady() {},
      send() { return true; },
      disconnectPeer() {},
      recv() { return undefined; }
    },
    'session-rid',
    undefined,
    service,
    completions
  );

  await stream.bindActor({
    actorId: 'actor-stale',
    objectGeneration: 1n,
    meshName: 'play',
    nodeRid: 'node-a'
  }, 1000);
  await assert.doesNotReject(() => stream.unbindActor('actor-stale', 1000));
});

test('ZLinkStreamBindingRuntime creates dotnet-shaped session context and closes through stream', async () => {
  let closed = 0;
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext({
    sessionId: 'session-1',
    routingId: 'rid-1',
    localAddr: 'tcp://local',
    remoteAddr: 'tcp://remote',
    write() {
      return true;
    },
    async close() {
      closed += 1;
    }
  });

  assert.equal(context.sessionId, 'session-1');
  assert.equal(context.routingId, 'rid-1');
  assert.equal(context.localAddr, 'tcp://local');
  assert.equal(context.remoteAddr, 'tcp://remote');
  await context.close();
  assert.equal(closed, 1);
});

test('built stream runtime rejects an unbound fallback disconnect without native or transport close', async () => {
  let nativeDisconnects = 0;
  let transportDisconnects = 0;
  const runtime = new framework.ZLinkStreamBindingRuntime({
    transport: {
      async send() {
        throw new Error('unbound fallback must not send through the transport');
      },
      async disconnect() {
        transportDisconnects += 1;
      }
    },
    nativeActorNodeProvider: () => ({
      async closeActorBoundSession() {
        nativeDisconnects += 1;
      }
    })
  });
  const session = new ZLinkNativeFallbackBoundSession({
    runtime,
    routedTransport: {},
    actorRefProvider: () => ({
      actorId: 'actor-unbound-runtime',
      objectGeneration: 1n,
      meshName: 'mesh',
      nodeRid: 'node-a',
      bindingGeneration: 0n
    }),
    nativeActorNodeProvider: () => ({
      async closeActorBoundSession() {
        nativeDisconnects += 1;
      }
    }),
    localActorProvider: () => true,
    remoteBoundSessionTargetProvider: () => undefined,
    remoteActorPacketTargetProvider: () => undefined,
    actorId: 'actor-unbound-runtime',
    reportError: () => undefined
  });

  await assert.rejects(
    () => session.disconnect(),
    error => error?.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
  );
  assert.equal(nativeDisconnects, 0);
  assert.equal(transportDisconnects, 0);
});

test('session actors bind actor refs, expose bound actors, and reject missing routing id', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(fakeStream('session-2', 'rid-2'));

  const actor = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-a',
    generation: 1
  });

  assert.equal(actor.actorId, 'actor-a');
  assert.equal(await context.actors.find('actor-a'), actor);
  assert.deepEqual(context.actors.bound.map((entry) => entry.actorId), ['actor-a']);

  const missingRoutingContext = runtime.createSessionContext(fakeStream('session-3', undefined));
  await assert.rejects(
    () => missingRoutingContext.actors.bind({ nodeRid: 'node-a', actorId: 'actor-b', generation: 1 }),
    /routing id/
  );
});

test('managed stream actor bind opens the exact native route before local binding is visible', async () => {
  const socket = new FakeStreamSocket();
  const runtime = new framework.ZLinkStreamBindingRuntime({ actorBindTimeoutMs: 1234 });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-a', generation: 1n };

  const actor = await context.actors.bind(actorRef);

  assert.equal(actor.actorId, 'actor-a');
  assert.equal(socket.boundActors.length, 1);
  assert.deepEqual(socket.boundActors[0], {
    sessionRid: 'backend-rid',
    actor: actorRef,
    timeoutMs: 1234
  });
  assert.equal(socket.boundActorSends.length, 0);
  assert.equal(await context.actors.find('actor-a'), actor);
  assert.equal(await runtime.find('actor-a'), actor);
});

test('managed stream actor bind uses the framework request timeout by default', async () => {
  const socket = new FakeStreamSocket();
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));

  await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-default-timeout', generation: 1n });

  assert.equal(socket.boundActors[0].timeoutMs, 30_000);
});

test('managed stream remote actor bind records the remote actor ref on the stream', async () => {
  const socket = new FakeStreamSocket();
  const node = new FakeSpotNode('node-local');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    actorBindTimeoutMs: 1234,
    nativeActorNodeProvider: () => node
  });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));
  const actorRef = { nodeRid: 'node-remote', actorId: 'actor-a', generation: 1n };

  const actor = await context.actors.bind(actorRef);

  assert.equal(actor.actorId, 'actor-a');
  assert.equal(socket.boundActors.length, 1);
  assert.deepEqual(socket.boundActors[0], {
    sessionRid: 'backend-rid',
    actor: actorRef,
    timeoutMs: 1234
  });
  assert.deepEqual(node.remoteSessionBinds, []);
  assert.equal(await context.actors.find('actor-a'), actor);
});

test('managed stream remote binding keeps the opaque backend session routing id', async () => {
  const socket = new FakeStreamSocket();
  const backendSessionRid = zlink.RoutingId.from(Buffer.from([0x00, 0x81, 0xfe, 0x7f]));
  const confirmedSessionRids = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    actorBindTimeoutMs: 1234,
    confirmRemoteActorSessionBinding: async (_actor, sessionRid) => {
      confirmedSessionRids.push(sessionRid);
    }
  });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, backendSessionRid));
  const actorRef = { nodeRid: 'node-remote', actorId: 'actor-opaque-session', generation: 1n };

  await context.actors.bind(actorRef);

  assert.equal(context.sessionId, backendSessionRid.toHex());
  assert.equal(socket.boundActors[0].sessionRid, backendSessionRid);
  assert.equal(confirmedSessionRids[0], backendSessionRid);
});

test('initial managed stream actor bind waits for remote Session route admission', async () => {
  const socket = new FakeStreamSocket();
  let releaseConfirmation;
  let confirmationStarted = false;
  const confirmation = new Promise((resolve) => {
    releaseConfirmation = resolve;
  });
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async confirmRemoteActorSessionBinding(_actor, _sessionRid, _signal, options) {
      confirmationStarted = true;
      assert.equal(options.waitForAcknowledgement, true);
      await confirmation;
    }
  });
  const context = runtime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session')
  );
  let completed = false;

  const binding = context.actors.bind({
    nodeRid: 'node-remote',
    actorId: 'actor-confirm-before-return',
    generation: 1n
  }).then(() => {
    completed = true;
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(confirmationStarted, true);
  assert.equal(completed, false);
  releaseConfirmation();
  await binding;
  assert.equal(completed, true);
});

test('remote actor session binding uses a non-correlated command over the actor mesh node route', async () => {
  const actorRef = {
    nodeRid: 'actor-node',
    actorId: 'actor-bind-command',
    objectGeneration: 3n,
    meshName: 'actor.route'
  };
  const routed = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'actor.route' }]
    })
  });
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, actorRef.actorId);
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'actor.route',
          targetNodeRid: actorRef.nodeRid,
          spotId: actorRef.nodeRid,
          spotKind: framework.ZLinkSpotKind.Entry
        }
      };
    }
  });
  host.routeTransport.request = async (meshName, targetNodeRid, packetName, request) => {
    routed.push({ meshName, targetNodeRid, packetName, request });
    return { ok: true, response: { acknowledged: true } };
  };

  await host.boundSessionRelay.actorPackets.confirmRemoteSessionBinding(
    actorRef,
    'session-node',
    'session-rid'
  );

  assert.equal(routed.length, 1);
  assert.equal(routed[0].meshName, 'actor.route');
  assert.equal(routed[0].targetNodeRid, 'actor-node');
  assert.equal(routed[0].packetName, framework.ZLINK_REMOTE_ACTOR_PACKET_RELAY_PACKET);
  const header = streamProtocol.decodeStreamHeader(Buffer.from(routed[0].request.header, 'base64'));
  assert.equal(header.name, 'framework.internal.actor-session-bind');
  assert.equal(header.kind, streamProtocol.ZLinkStreamMessageKind.Send);
  assert.equal(header.flags, streamProtocol.ZLinkStreamHeaderFlags.None);
  assert.equal(header.requestSeq, undefined);
  assert.equal(routed[0].request.boundSessionTargetNodeRid, 'session-node');
  assert.equal(routed[0].request.boundSessionSpotId, 'session-node');
});

test('one-way remote actor session bind completes only after its route send is submitted', async () => {
  const actorRef = {
    nodeRid: 'actor-node',
    actorId: 'actor-bind-submit-order',
    objectGeneration: 3n,
    meshName: 'actor.route'
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'actor.route' }]
    })
  });
  host.setActorManager({
    getState() {
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'actor.route',
          targetNodeRid: actorRef.nodeRid,
          spotId: actorRef.nodeRid,
          spotKind: framework.ZLinkSpotKind.Entry
        }
      };
    }
  });
  let releaseSend;
  const sendSubmitted = new Promise((resolve) => { releaseSend = resolve; });
  host.routeTransport.sendToSpot = async () => { await sendSubmitted; };
  let completed = false;

  const binding = host.boundSessionRelay.actorPackets.confirmRemoteSessionBinding(
    actorRef,
    'session-node',
    'session-rid',
    undefined,
    { waitForAcknowledgement: false }
  ).then(() => { completed = true; });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(completed, false);
  releaseSend();
  await binding;
  assert.equal(completed, true);
});

test('acknowledged remote actor session bind nack surfaces the remote failure classification', async () => {
  //  Spec 32-framework-error-model: an immediate {ok:false} reply is a remote
  //  rejection, not a deadline elapse — DeadlineExceeded(7) is reserved for
  //  failing to complete within the deadline. The reply's own errorKind is
  //  decoded when valid; a reply without one maps to RequestFailed, matching
  //  the acknowledged-reply convention (actorRelayError/remoteRelayErrorKind).
  const actorRef = {
    nodeRid: 'actor-node',
    actorId: 'actor-bind-deadline',
    objectGeneration: 3n,
    meshName: 'actor.route'
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'actor.route' }]
    })
  });
  host.setActorManager({
    getState() {
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'actor.route',
          targetNodeRid: actorRef.nodeRid,
          spotId: actorRef.nodeRid,
          spotKind: framework.ZLinkSpotKind.Entry
        }
      };
    }
  });
  const lastFailure = 'actor does not hold a current session binding';
  let replyErrorKind = framework.ZLinkFrameworkInternalErrorKind.ActorSessionNotBound;
  host.routeTransport.request = async () => ({
    ok: false,
    error: lastFailure,
    ...(replyErrorKind === undefined ? {} : { errorKind: replyErrorKind })
  });

  await assert.rejects(
    host.boundSessionRelay.actorPackets.confirmRemoteSessionBinding(
      actorRef,
      'session-node',
      'session-rid'
    ),
    (error) => {
      assert.ok(error instanceof framework.ZLinkFrameworkException);
      //  The remote reply's own kind is decoded — never DeadlineExceeded here.
      assert.notEqual(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
      assert.equal(
        framework.internalFrameworkErrorKind(error),
        framework.ZLinkFrameworkInternalErrorKind.ActorSessionNotBound
      );
      assert.equal(error.cause, lastFailure);
      assert.match(error.message, /did not acknowledge its remote session binding/);
      return true;
    }
  );

  //  A nack without a decodable errorKind still stays off the deadline
  //  classification and falls back to RequestFailed.
  replyErrorKind = undefined;
  await assert.rejects(
    host.boundSessionRelay.actorPackets.confirmRemoteSessionBinding(
      actorRef,
      'session-node',
      'session-rid'
    ),
    (error) => {
      assert.ok(error instanceof framework.ZLinkFrameworkException);
      assert.notEqual(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      assert.equal(
        framework.internalFrameworkErrorKind(error),
        framework.ZLinkFrameworkInternalErrorKind.RequestFailed
      );
      return true;
    }
  );
});

test('initial managed stream actor bind removes its provisional route and surfaces an acknowledged confirmation failure', async () => {
  // Spec 20: the first bind has no current route to preserve. It only becomes
  // current after the Actor owner accepts it, so the relay's typed failure is
  // the public bind failure and the provisional native/local binding is gone.
  const socket = new FakeStreamSocket();
  const confirmationFailure = new framework.ZLinkFrameworkException(
    framework.ZLinkFrameworkErrorKind.InvalidOperation,
    'remote session binding was rejected'
  );
  const runtime = new framework.ZLinkStreamBindingRuntime({
    confirmRemoteActorSessionBinding: async (_actor, _sessionRid, _signal, options) => {
      assert.equal(options.waitForAcknowledgement, true);
      throw confirmationFailure;
    }
  });
  const context = runtime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session')
  );

  await assert.rejects(
    () => context.actors.bind({
      nodeRid: 'node-remote',
      actorId: 'actor-confirm-nack-observed',
      generation: 1n
    }),
    error => error === confirmationFailure
  );
  assert.equal(await context.actors.find('actor-confirm-nack-observed'), undefined);
  assert.equal(await runtime.find('actor-confirm-nack-observed'), undefined);
  assert.deepEqual(socket.unboundActors, ['actor-confirm-nack-observed']);
});

test('fire-and-forget remote actor session bind retry exhaustion reports a typed DeadlineExceeded', async () => {
  //  Spec 32-framework-error-model: DeadlineExceeded(7) — classification only;
  //  the fire-and-forget send still reports through the error sink instead of
  //  throwing.
  const reported = [];
  const sendFailure = new Error('route send failed');
  const relay = new ZLinkActorPacketRelay({
    routeTransport: {
      sendToSpot: async () => { throw sendFailure; }
    },
    streamBindingRuntime: () => ({ find() {} }),
    meshRouters: {},
    actorManager: () => undefined,
    spotManager: () => undefined,
    spotNodeRuntime: () => undefined,
    errorSink: () => ({
      reportRuntimeTaskException(taskName, error) {
        reported.push({ taskName, error });
      }
    })
  });

  //  Enter the retry loop with the deadline already elapsed so exhaustion is
  //  observed without altering the production retry timing or bounds.
  relay.retryRemoteSessionBindingSend(
    { routerChannelId: 'actor.route', targetNodeRid: 'actor-node', spotId: 'actor-node' },
    { actorId: 'actor-bind-deadline' },
    Date.now() - 1,
    1
  );
  await new Promise((resolve) => setImmediate(resolve));
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(reported.length, 1);
  assert.equal(reported[0].taskName, 'remote session binding send');
  const error = reported[0].error;
  assert.ok(error instanceof framework.ZLinkFrameworkException);
  assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  assert.equal(
    framework.internalFrameworkErrorKind(error),
    framework.ZLinkFrameworkInternalErrorKind.DeadlineExceeded
  );
  assert.equal(error.cause, sendFailure);
  assert.match(error.message, /retries exceeded their deadline/);
});

test('remote actor session binding keeps its declared return route before peer discovery catches up', async () => {
  let remoteTarget;
  const actorRef = { nodeRid: 'actor-node', actorId: 'actor-bind-route', generation: 4n };
  const relay = new ZLinkActorPacketRelay({
    routeTransport: {},
    streamBindingRuntime: () => ({ find() {} }),
    meshRouters: {
      remoteBoundSessionTargetForSource() { return undefined; }
    },
    actorManager: () => ({
      getState() {
        return {
          nativeActorRef: actorRef,
          setRemoteBoundSessionTarget(value) { remoteTarget = value; }
        };
      }
    }),
    spotManager: () => undefined,
    spotNodeRuntime: () => ({
      primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
        status: () => ({ routingId: zlink.RoutingId.from('actor-node') })
      }
    }),
    errorSink: () => ({ reportRuntimeTaskException() {} })
  });
  const payload = actorPacketWire.encodeRemoteActorPacketRelayPayload({
    actorId: actorRef.actorId,
    routerChannelId: 'actor.route',
    header: streamProtocol.encodeStreamHeader({
      kind: streamProtocol.ZLinkStreamMessageKind.Send,
      codec: streamProtocol.ZLinkStreamCodec.Raw,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      name: actorPacketWire.ZLINK_REMOTE_ACTOR_SESSION_BIND_PACKET,
      metadata: new Map()
    }),
    payload: actorPacketWire.encodeRemoteActorSessionBinding({
      sessionNodeRid: 'session-node',
      sessionRid: 'session-rid'
    })
  });

  const reply = await relay.receiveRemoteActorPacketRelay(payload, { sourceNodeRid: 'session-node' });

  assert.deepEqual(reply, { ok: true, response: { acknowledged: true } });
  assert.deepEqual(remoteTarget, {
    routerChannelId: 'actor.route',
    targetNodeRid: 'session-node',
    spotId: 'session-node',
    sessionNodeRid: 'session-node',
    sessionRid: 'session-rid'
  });
});

test('managed stream local actor bind does not relay a remote ownership marker', async () => {
  const socket = new FakeStreamSocket();
  const node = new FakeSpotNode('node-local');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    nativeActorNodeProvider: () => node
  });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));

  await context.actors.bind({ nodeRid: 'node-local', actorId: 'actor-local', generation: 1n });

  assert.equal(socket.boundActors.length, 1);
  assert.equal(socket.boundActorSends.length, 0);
});

test('managed stream actor rebind is idempotent for the same actor ref', async () => {
  const socket = new FakeStreamSocket();
  const runtime = new framework.ZLinkStreamBindingRuntime({ actorBindTimeoutMs: 1234 });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-a', generation: 1n };

  await context.actors.bind(actorRef);
  await runtime.rebindActor(actorRef);

  assert.equal(socket.boundActors.length, 1);
  assert.deepEqual(socket.boundActors[0], {
    sessionRid: 'backend-rid',
    actor: actorRef,
    timeoutMs: 1234
  });
});

test('managed stream actor route commit rebinds the native gateway for a new owner', async () => {
  const socket = new FakeStreamSocket();
  const runtime = new framework.ZLinkStreamBindingRuntime({ actorBindTimeoutMs: 1234 });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-a', generation: 1n };

  await context.actors.bind(actorRef);
  await runtime.commitActorRoute({ ...actorRef, nodeRid: 'node-b' });

  assert.equal(socket.boundActors.length, 2);
  assert.deepEqual(socket.boundActors[1], {
    sessionRid: 'backend-rid',
    actor: { ...actorRef, nodeRid: 'node-b' },
    timeoutMs: 1234
  });
  assert.equal(socket.boundActorSends.length, 0);
});

test('managed stream actor bind failure does not create stale local binding', async () => {
  const socket = new FakeStreamSocket();
  const typedFailure = new framework.ZLinkFrameworkException(
    framework.ZLinkFrameworkErrorKind.DeadlineExceeded,
    'native bind deadline exceeded'
  );
  socket.bindError = typedFailure;
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));

  await assert.rejects(
    () => context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 1n }),
    error => {
      assert.equal(error, typedFailure);
      assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      return true;
    }
  );

  assert.equal(await context.actors.find('actor-a'), undefined);
  assert.equal(await runtime.find('actor-a'), undefined);
});

test('initial managed stream remote bind deadline failure does not leave a provisional binding', async () => {
  const operations = [];
  let nativeActor;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(_sessionRid, actor) {
      operations.push(`bind:${actor.actorId}`);
      nativeActor = actor;
    },
    async unbindActor(_sessionRid, actorId) {
      operations.push(`unbind:${actorId}`);
      nativeActor = undefined;
    },
    sendBoundActor() { return true; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async confirmRemoteActorSessionBinding() {
      throw new Error('remote bound session bind confirmation failed');
    }
  });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid'));

  await assert.rejects(
    () => context.actors.bind({
      nodeRid: 'remote-node',
      actorId: 'actor-relay-fail',
      generation: 1n
    }),
    /remote bound session bind confirmation failed/
  );

  assert.equal(nativeActor, undefined);
  assert.equal(await context.actors.find('actor-relay-fail'), undefined);
  assert.equal(await runtime.find('actor-relay-fail'), undefined);
  assert.deepEqual(operations, ['bind:actor-relay-fail', 'unbind:actor-relay-fail']);
});

test('runtime host bound session uses local stream route before native SessionRelay', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native', generation: 7n };
  const nativeSends = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession(actor, bindingGeneration, parts, flags) {
        nativeSends.push({ actor, bindingGeneration, frame: decodeFrame(bytesOf(parts[0])), flags });
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession() {}
    }
  };

  const stream = recordingStream('session-native', 'rid-native');
  const context = host.streamBindingRuntime.createSessionContext(stream);
  await context.actors.bind(actorRef);

  const submit = host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  await submit;
  assert.equal(nativeSends.length, 0);
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(frame.header.name, 'Notify');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { ok: true });
});

test('runtime host local actor falls back to native SessionRelay when no JavaScript route exists', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native', generation: 7n };
  const nativeSends = [];
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.send = (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({ routerChannelId, targetNodeRid, packetName, message, signal });
    return Promise.resolve({ ok: true });
  };
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession(actor, bindingGeneration, parts, flags) {
        nativeSends.push({ actor, bindingGeneration, frame: decodeFrame(bytesOf(parts[0])), flags });
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession() {}
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            actor: { actorId: actorRef.actorId },
            nativeActorRef: actorRef,
            boundSessionBindingGeneration: 11n
          }
        : undefined;
    }
  });

  const result = await host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(result, undefined);
  assert.equal(nativeSends.length, 1);
  assert.equal(routeCalls.length, 0);
  assert.deepEqual(nativeSends[0].actor, actorRef);
  assert.equal(nativeSends[0].bindingGeneration, 11n);
  assert.equal(nativeSends[0].flags, 1);
  assert.equal(nativeSends[0].frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(nativeSends[0].frame.header.name, 'Notify');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(nativeSends[0].frame.payload)), { ok: true });
});

test('native bound-session send awaits binding admission without Framework retry ownership', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-ready', generation: 7n };
  let attempts = 0;
  let releaseAdmission;
  const admission = new Promise((resolve) => { releaseAdmission = resolve; });
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        attempts += 1;
        return admission;
      }
    }
  };
  host.setActorManager({
    getState() {
      return {
        actor: { actorId: actorRef.actorId },
        nativeActorRef: actorRef,
        boundSessionBindingGeneration: 11n
      };
    }
  });

  const submit = host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  await new Promise((resolve) => setTimeout(resolve, 25));
  assert.equal(attempts, 1, 'Framework must await the binding admission Promise once');
  releaseAdmission(zlink.SubmitResult.Ok);

  assert.equal(await submit, undefined);
  assert.equal(attempts, 1);
});

test('native bound-session send does not add an abort owner after binding admission starts', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-cancel', generation: 7n };
  let attempts = 0;
  let releaseAdmission;
  const admission = new Promise((resolve) => { releaseAdmission = resolve; });
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        attempts += 1;
        return admission;
      }
    }
  };
  host.setActorManager({
    getState() {
      return {
        actor: { actorId: actorRef.actorId },
        nativeActorRef: actorRef,
        boundSessionBindingGeneration: 12n
      };
    }
  });
  const controller = new AbortController();
  const submit = host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit(controller.signal);

  await waitForCondition(() => attempts === 1, 'first native bound-session admission');
  controller.abort();
  releaseAdmission(zlink.SubmitResult.Ok);
  assert.equal(await submit, undefined);
  assert.equal(attempts, 1);
});

test('native bound-session send propagates binding admission failure without retry', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-timeout', generation: 7n };
  let attempts = 0;
  const bindingFailure = new Error('binding admission timed out');
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      spotNodes: {
        play: {
          router: {
            bind: 'inproc://native-bound-session-timeout',
            sendTimeoutMs: 5
          }
        }
      }
    })
  });
  host.spotNodeRuntime = {
    primaryMeshName: 'play',
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        attempts += 1;
        return Promise.reject(bindingFailure);
      }
    }
  };
  host.setActorManager({
    getState() {
      return {
        actor: { actorId: actorRef.actorId },
        nativeActorRef: actorRef,
        boundSessionBindingGeneration: 13n
      };
    }
  });

  await assert.rejects(
    () => host.createActorManagerOptions()
      .boundSessionFactory(actorRef.actorId)
      .send({ ok: true })
      .packetName('Notify')
      .submit(),
    (error) => error === bindingFailure
  );
  assert.equal(attempts, 1);
});

test('runtime host bound session uses routed Session target before native SessionRelay', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-routed', generation: 7n };
  const sessionNodeRid = zlink.RoutingId.from('session-node');
  const sessionSpotId = zlink.RoutingId.from('session-entry');
  const nativeSends = [];
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      message,
      signal
    });
    return { ok: true };
  };
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession(actor, parts, flags) {
        nativeSends.push({ actor, parts, flags });
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession() {}
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: sessionNodeRid,
              spotId: sessionSpotId
            }
          }
        : undefined;
    }
  });

  const submit = host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(nativeSends.length, 0);
  await waitForCondition(() => routeCalls.length === 1, 'routed bound session submit');
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(String(routeCalls[0].targetNodeRid), 'session-node');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].message.boundPacketName, 'Notify');
});

test('runtime host local actor uses its native binding before a transfer target', async () => {
  const actorRef = { nodeRid: 'node-local', actorId: 'actor-native-bound', generation: 7n };
  const nativeSends = [];
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.submit = async (...args) => {
    routeCalls.push(args);
    return { status: framework.ZLinkSubmitStatus.Submitted };
  };
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession(actor, bindingGeneration, parts, flags) {
        nativeSends.push({ actor, bindingGeneration, parts, flags });
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession() {}
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            actor: { actorId },
            nativeActorRef: actorRef,
            boundSessionBindingGeneration: 11n,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: 'session-node',
              spotId: 'session-entry'
            }
          }
        : undefined;
    }
  });

  await host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(nativeSends.length, 1);
  assert.equal(nativeSends[0].bindingGeneration, 11n);
  assert.equal(routeCalls.length, 0);
});

test('runtime host local relocating actor uses its exact sealed Session owner before native binding', async () => {
  const actorRef = { nodeRid: 'node-local', actorId: 'actor-sealed-bound', generation: 7n };
  const nativeSends = [];
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.submitInfrastructure = async (...args) => {
    routeCalls.push(args);
    return { status: ZLinkSubmitStatus.Submitted };
  };
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession(actor, bindingGeneration, parts, flags) {
        nativeSends.push({ actor, bindingGeneration, parts, flags });
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession() {}
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            actor: { actorId },
            nativeActorRef: actorRef,
            boundSessionBindingGeneration: 11n,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: 'session-node',
              spotId: 'session-entry',
              relocationSealId: 'seal-exact'
            }
          }
        : undefined;
    }
  });

  await host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(nativeSends.length, 0);
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0][3].relocationSealId, 'seal-exact');
});

test('runtime host bound session disconnect uses routed Session target before native SessionRelay', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-routed-disconnect', generation: 7n };
  const sessionNodeRid = zlink.RoutingId.from('session-node');
  const sessionSpotId = zlink.RoutingId.from('session-entry');
  const nativeDisconnects = [];
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.requestRawToSpot = async (remoteAddress, request, options) => {
    const message = JSON.parse(request.getString('utf8'));
    routeCalls.push({
      routerChannelId: remoteAddress.routerChannelId,
      targetNodeRid: remoteAddress.targetNodeRid,
      spotId: remoteAddress.spotId,
      packetName: message.packetName,
      message,
      signal: options.signal
    });
    return [bindingMessage(JSON.stringify({ ok: true }))];
  };
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        return zlink.SubmitResult.Ok;
      },
      async closeActorBoundSession(actor) {
        nativeDisconnects.push(actor);
      }
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: sessionNodeRid,
              spotId: sessionSpotId
            }
          }
        : undefined;
    }
  });

  host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .disconnect();

  assert.equal(nativeDisconnects.length, 0);
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(String(routeCalls[0].targetNodeRid), 'session-node');
  assert.equal(String(routeCalls[0].spotId), 'session-entry');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.packet.relay');
  const header = streamProtocol.decodeStreamHeader(Buffer.from(routeCalls[0].message.header, 'base64'));
  assert.equal(header.name, 'zlink.framework.actor.session_disconnected');
  assert.equal(header.kind, streamProtocol.ZLinkStreamMessageKind.Send);
  assert.equal(header.codec, streamProtocol.ZLinkStreamCodec.Raw);
});

test('runtime host bound session uses routed Session target before stale local route', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-routed-local', generation: 7n };
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      message
    });
    return { ok: true };
  };
  const stream = recordingStream('session-stale', 'rid-stale');
  const context = host.streamBindingRuntime.createSessionContext(stream);
  await context.actors.bind(actorRef);
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: 'session-node',
              spotId: 'session-entry'
            }
          }
        : undefined;
    }
  });

  host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(stream.writes.length, 0);
  await waitForCondition(() => routeCalls.length === 1, 'routed bound session submit');
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].message.boundPacketName, 'Notify');
});

test('runtime host local actor keeps its current stream route when a remote packet target is present', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-local-current', generation: 7n };
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.requestToSpot = async (...args) => {
    routeCalls.push(args);
    return { ok: true };
  };
  const stream = recordingStream('session-current', 'rid-current');
  const context = host.streamBindingRuntime.createSessionContext(stream);
  await context.actors.bind(actorRef);
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            actor: { actorId: actorRef.actorId },
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: 'remote-node',
              spotId: 'remote-entry'
            }
          }
        : undefined;
    }
  });

  const submit = host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  await submit;
  assert.equal(stream.writes.length, 1);
  assert.equal(routeCalls.length, 0);
});

test('runtime host bound session uses Spot route target even when route channel send is available', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-routed-rid', generation: 7n };
  const routeSends = [];
  const spotSends = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: ['room.route']
    })
  });
  host.routeTransport.send = (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeSends.push({ routerChannelId, targetNodeRid, packetName, message, signal });
    return Promise.resolve();
  };
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, request, signal) => {
    spotSends.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      message: request,
      signal
    });
    return { ok: true };
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId
        ? {
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'room.route',
              targetNodeRid: zlink.RoutingId.from('session-node'),
              spotId: zlink.RoutingId.from('session-entry')
            }
          }
        : undefined;
    }
  });

  host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  assert.equal(routeSends.length, 0);
  await waitForCondition(() => spotSends.length === 1, 'Spot bound session submit');
  assert.equal(spotSends.length, 1);
  assert.equal(spotSends[0].routerChannelId, 'room.route');
  assert.equal(String(spotSends[0].targetNodeRid), 'session-node');
  assert.equal(spotSends[0].packetName, '__zlink.actor.bound_session.send');
});

test('runtime host local spot join preserves routed Session target for stream-bound actor', async () => {
  class PlayerActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
    }
  }
  class PlayerFactory {
    async create(context) {
      return new PlayerActor(context.actorId, context);
    }
  }

  const actorRef = { nodeRid: zlink.RoutingId.from('play-node'), actorId: 'actor-routed-local-join', generation: 7n };
  const remoteTarget = {
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry')
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const manager = new framework.DefaultZLinkActorManager({
    actorFactories: new Map([['player', PlayerFactory]])
  });
  host.setActorManager(manager);
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      status: () => ({ routingId: zlink.RoutingId.from('play-node') }),
      actorLookup() {
        return undefined;
      },
      createActor(actorId) {
        return { nodeRid: zlink.RoutingId.from('play-node'), actorId, generation: 1n };
      }
    }
  };

  await manager.getOrCreateWithNativeRef('actor-routed-local-join', 'player', actorRef);
  manager.getState('actor-routed-local-join').setRemoteBoundSessionTarget(remoteTarget);

  await host.createSpotManagerOptions().actorTransferRuntime.getOrCreateRoutedActor(
    'actor-routed-local-join',
    'player'
  );

  assert.deepEqual(manager.getState('actor-routed-local-join').remoteBoundSessionTarget, remoteTarget);
});

test('runtime host local actor uses its current routed session target', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native', generation: 7n };
  const routed = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, payload) => {
    routed.push({ routerChannelId, targetNodeRid, packetName, payload });
    return { ok: true };
  };
  host.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({ actorBindTimeoutMs: 100 });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        throw new Error('native binding must not replace the current routed session target');
      },
      async closeActorBoundSession() {}
    }
  };
  host.setActorManager({
    getState(actorId) {
      return actorId === actorRef.actorId ? {
        actor: { actorId: actorRef.actorId },
        nativeActorRef: actorRef,
        remoteBoundSessionTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'session-node',
          spotId: 'session-entry'
        }
      } : undefined;
    }
  });

  host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

  await waitForCondition(() => routed.length === 1, 'routed bound session send');
  assert.equal(routed[0].routerChannelId, 'room.route');
  assert.equal(routed[0].targetNodeRid, 'session-node');
  assert.equal(routed[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routed[0].payload.boundPacketName, 'Notify');
  assert.deepEqual(routed[0].payload.message, { ok: true });
});

test('local actor push preserves native target-not-found after its remote session route is removed', async () => {
  const actorRef = { nodeRid: 'actor-node', actorId: 'actor-disconnected-local', generation: 1n };
  let nativeAttempts = 0;
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({ actorBindTimeoutMs: 1 });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        nativeAttempts += 1;
        return zlink.SubmitResult.NotFound;
      }
    }
  };
  host.setActorManager({
    getState() {
      return {
        actor: { actorId: actorRef.actorId },
        nativeActorRef: actorRef,
        boundSessionBindingGeneration: 11n
      };
    }
  });
  await assert.rejects(
    () => host.createActorManagerOptions()
      .boundSessionFactory(actorRef.actorId)
      .send({ stale: true })
      .packetName('Notify')
      .submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation
  );
  assert.equal(nativeAttempts, 1);
});

test('actor response compression reaches local, native, and remote bound-session transports', async () => {
  const streamCalls = [];
  const remotePayloads = [];
  const remoteTargets = [];
  let localAccepted = true;
  let remoteBoundSessionTarget;
  let actorStateAvailable = true;
  const streamRuntime = {
    sendLocalBoundSessionResponse(...args) {
      streamCalls.push({ kind: 'local', args });
      return localAccepted;
    },
    async sendNativeBoundSessionResponse(...args) {
      streamCalls.push({ kind: 'native', args });
    }
  };
  const relay = new ZLinkRemoteBoundSessionRelay({
    routeTransport: {
      async submit(routerChannelId, targetNodeRid, packetName, payload) {
        remoteTargets.push({ routerChannelId, targetNodeRid, packetName });
        remotePayloads.push(payload);
        return { status: 'submitted' };
      }
    },
    streamBindingRuntime: () => streamRuntime,
    actorManager: () => ({
      getState() {
        if (!actorStateAvailable) return undefined;
        return {
          nativeActorRef: { nodeRid: 'node-a', actorId: 'actor-compress', objectGeneration: 1n },
          meshName: 'route-main',
          boundSessionBindingGeneration: 1n,
          remoteBoundSessionTarget
        };
      }
    }),
    actorSessionNode: () => ({}),
    destroyedActorRefs: new Map(),
    boundSessionFactory() {
      throw new Error('bound session factory must not be used by response delivery');
    },
    updateRemoteActorPacketTarget() {},
    actorPacketTargetForState: () => undefined
  });
  const actor = { context: { actorId: 'actor-compress' } };
  const replyOptions = {
    metadata: new Map([['reply-trace-id', 'reply:actor-compress']]),
    compressPayload: true
  };

  await relay.sendActorResponse(actor, 'Move', 41n, { accepted: 'local' }, replyOptions);
  assert.equal(streamCalls[0].kind, 'local');
  assert.equal(streamCalls[0].args[5], true);

  localAccepted = false;
  await relay.sendActorResponse(actor, 'Move', 42n, { accepted: 'native' }, replyOptions);
  assert.equal(streamCalls[2].kind, 'native');
  assert.equal(streamCalls[2].args[6], true);

  remoteBoundSessionTarget = {
    routerChannelId: 'route-main',
    targetNodeRid: 'node-b',
    spotId: 'entry-b'
  };
  await relay.sendActorResponse(actor, 'Move', 43n, { accepted: 'remote' }, replyOptions);
  assert.equal(remotePayloads.length, 1);
  assert.equal(remotePayloads[0].compressPayload, true);
  assert.deepEqual(remotePayloads[0].metadata, { 'reply-trace-id': 'reply:actor-compress' });

  actorStateAvailable = false;
  await relay.sendActorResponse(
    actor,
    'Move',
    44n,
    { accepted: 'after-transfer' },
    replyOptions,
    remoteBoundSessionTarget,
    {
      nodeRid: 'node-a',
      actorId: 'actor-compress',
      objectGeneration: 1n,
      meshName: 'route-main'
    }
  );
  assert.equal(remotePayloads.length, 2);
  assert.deepEqual(remotePayloads[1].message, { accepted: 'after-transfer' });

  actorStateAvailable = true;
  const requestBoundSessionTarget = {
    routerChannelId: 'request-route',
    targetNodeRid: 'request-node',
    spotId: 'request-entry'
  };
  await relay.sendActorResponse(
    actor,
    'Move',
    45n,
    { accepted: 'request-route' },
    replyOptions,
    requestBoundSessionTarget
  );
  assert.equal(remoteTargets.at(-1).routerChannelId, requestBoundSessionTarget.routerChannelId);
  assert.equal(remoteTargets.at(-1).targetNodeRid, requestBoundSessionTarget.targetNodeRid);
  assert.equal(remoteTargets.at(-1).packetName, framework.ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET);

  localAccepted = true;
  await relay.receiveRemoteBoundSessionResponse({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET,
    actorId: 'actor-compress',
    boundPacketName: 'Move',
    requestSeq: '44',
    message: { accepted: 'remote-received' },
    metadata: { 'reply-trace-id': 'reply:remote-received' },
    compressPayload: true
  });
  const receivedCall = streamCalls.at(-1);
  assert.equal(receivedCall.kind, 'local');
  assert.equal(receivedCall.args[5], true);
});

test('runtime host remote bound session send submits a routed Session command', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const routeCalls = [];
  host.routeTransport.requestToSpot = async () => {
    throw new Error('bound session send must not wait for a routed reply');
  };
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      packet: message,
      signal
    });
    return { status: ZLinkSubmitStatus.Submitted };
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote');
      return {
        remoteBoundSessionTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'session-node',
          spotId: 'session-entry'
        }
      };
    }
  });

  const result = await host.createActorManagerOptions()
    .boundSessionFactory('actor-remote')
    .send({ hello: 'world' })
    .packetName('Notify')
    .submit();

  assert.equal(result, undefined);
  await waitForCondition(() => routeCalls.length === 1, 'remote bound session route submit');
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(routeCalls[0].targetNodeRid, 'session-node');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].packet.packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].packet.actorId, 'actor-remote');
  assert.equal(routeCalls[0].packet.boundPacketName, 'Notify');
});

test('runtime host remote bound session receiver forwards through actor remote target', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const routeCalls = [];
  let resolveRoute;
  const routeReleased = new Promise((resolve) => {
    resolveRoute = resolve;
  });
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      packet: message,
      signal
    });
    await routeReleased;
    return { ok: true };
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-hop');
      return {
        remoteBoundSessionTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'session-node',
          spotId: 'session-entry'
        }
      };
    }
  });

  let completed = false;
  const received = host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSend({
    packetName: '__zlink.actor.bound_session.send',
    actorId: 'actor-hop',
    message: { hello: 'world' },
    boundPacketName: 'Notify',
    metadata: { seq: '1' }
  }).then(() => {
    completed = true;
  });

  await waitForCondition(() => routeCalls.length === 1, 'remote bound session route submit');
  assert.equal(completed, false);
  resolveRoute();
  await received;
  assert.equal(completed, true);
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(routeCalls[0].targetNodeRid, 'session-node');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].packet.actorId, 'actor-hop');
  assert.equal(routeCalls[0].packet.boundPacketName, 'Notify');
  assert.deepEqual(routeCalls[0].packet.message, { hello: 'world' });
  assert.deepEqual(routeCalls[0].packet.metadata, { seq: '1' });
});

test('runtime host remote bound session receiver delivers to local stream before forwarding', async () => {
  const actorRef = { nodeRid: 'session-node', actorId: 'actor-local-hop', generation: 1n };
  const routeCalls = [];
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.routeTransport.requestRawToSpot = async (remoteAddress, request, options) => {
    routeCalls.push({
      remoteAddress,
      message: JSON.parse(request.getString('utf8')),
      options
    });
    return [bindingMessage(JSON.stringify({ ok: true }))];
  };
  const stream = recordingStream('session-local-hop', 'rid-local-hop');
  const context = host.streamBindingRuntime.createSessionContext(stream);
  await context.actors.bind(actorRef);
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-local-hop');
      return {
        nativeActorRef: actorRef,
        remoteBoundSessionTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'other-session-node',
          spotId: 'other-session-entry'
        }
      };
    }
  });

  await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSend({
    packetName: '__zlink.actor.bound_session.send',
    actorId: 'actor-local-hop',
    message: { hello: 'local' },
    boundPacketName: 'Notify',
    metadata: { seq: '2' }
  });

  assert.equal(routeCalls.length, 0);
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(frame.header.name, 'Notify');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { hello: 'local' });
});

test('remote bound session receiver completes a stale best-effort delivery without forwarding', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.setActorManager({
    getState() {
      return {
        nativeActorRef: { nodeRid: 'actor-node', actorId: 'actor-disconnected', generation: 1n }
      };
    }
  });

  assert.deepEqual(
    await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSend({
      packetName: '__zlink.actor.bound_session.send',
      actorId: 'actor-disconnected',
      message: { hello: 'stale-session' },
      boundPacketName: 'Notify',
      metadata: {}
    }),
    { ok: false }
  );
  await host.createSpotManagerOptions().boundSessionRuntime.receiveRoutedBoundSession(
    'actor-disconnected',
    { hello: 'stale-session' },
    'Notify',
    new Map()
  );
});

test('routed bound-session receive does not regress a newer ownership generation', async () => {
  let releaseOldTargetUpdate;
  const oldTargetUpdate = new Promise((resolve) => { releaseOldTargetUpdate = resolve; });
  const rebound = [];
  const delivered = [];
  const relay = new ZLinkRemoteBoundSessionRelay({
    async updateRemoteActorPacketTarget(_actorId, target) {
      if (target === 'old') await oldTargetUpdate;
    },
    streamBindingRuntime() {
      return {
        async rebindActor(actorRef) { rebound.push(actorRef.ownershipGeneration); },
        async sendLocalBoundSession(_actorId, message) {
          delivered.push(message.marker);
          return true;
        }
      };
    },
    actorManager: () => undefined,
    boundSessionFactory() {
      throw new Error('stale receive must not fall back');
    }
  });
  const oldReceive = relay.receiveRoutedBoundSession(
    'actor-generation-fence', { marker: 'old' }, 'Notify', new Map(),
    { actorId: 'actor-generation-fence', ownershipGeneration: 1n }, 'old'
  );
  await new Promise((resolve) => setImmediate(resolve));
  await relay.receiveRoutedBoundSession(
    'actor-generation-fence', { marker: 'new' }, 'Notify', new Map(),
    { actorId: 'actor-generation-fence', ownershipGeneration: 2n }, 'new'
  );
  releaseOldTargetUpdate();
  await oldReceive;

  assert.deepEqual(rebound, [2n]);
  assert.deepEqual(delivered, ['new']);
});

test('routed target push refreshes a bound session to the transferred actor ref before delivery', async () => {
  const sourceRef = {
    nodeRid: 'actor-a', actorId: 'actor-transfer', generation: 1n,
    ownershipGeneration: 1n, ownerLeaseGeneration: 3n,
    bindingGeneration: 7n, acceptedHighWater: 9n
  };
  const targetRef = {
    nodeRid: 'actor-b',
    actorId: 'actor-transfer',
    generation: 1n,
    ownershipGeneration: 2n,
    ownerLeaseGeneration: 4n,
    bindingGeneration: 7n,
    acceptedHighWater: 9n
  };
  const staleSourceRef = { ...sourceRef, ownershipGeneration: 1n };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const stream = recordingStream('session-transfer', 'session-a');
  const context = host.streamBindingRuntime.createSessionContext(stream);
  await context.actors.bind(sourceRef);
  const refreshed = [];
  const rebound = [];
  const packetTargets = [];
  host.boundSessionRelay.actorPackets.updateRemoteActorPacketTarget = (actorId, target) => {
    packetTargets.push({ actorId, target });
  };
  const commitActorRoute = host.streamBindingRuntime.commitActorRoute.bind(
    host.streamBindingRuntime
  );
  host.streamBindingRuntime.commitActorRoute = async (...args) => {
    const [actorRef] = args;
    refreshed.push(actorRef);
    await commitActorRoute(...args);
  };
  host.streamBindingRuntime.rebindActor = async (actorRef) => {
    rebound.push(actorRef);
  };

  const seal = serviceSessionRelocationSeal('actor-transfer', {
    actorGeneration: 1n,
    sourceNodeRid: 'actor-a',
    authorityOwnerGeneration: 1n,
    ownerLeaseGeneration: 3n,
    sessionRid: 'session-a',
    bindingGeneration: 7n
  });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(
    serviceSessionRelocationRoute(seal, {
      targetNodeRid: 'actor-b',
      targetAuthorityOwnerGeneration: 2n
    })
  );
  await host.boundSessionRelay.boundSessions.receiveRoutedBoundSession(
    'actor-transfer',
    { marker: 'stale-source-before-target-push' },
    'Notify',
    new Map(),
    staleSourceRef
  );
  await host.boundSessionRelay.boundSessions.receiveRoutedBoundSession(
    'actor-transfer',
    { marker: 'after-transfer' },
    'Notify',
    new Map(),
    targetRef,
    {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-b',
      spotId: 'zone-sw',
      spotKind: framework.ZLinkSpotKind.User
    }
  );
  await host.boundSessionRelay.boundSessions.receiveRoutedBoundSession(
    'actor-transfer',
    { marker: 'stale-source' },
    'Notify',
    new Map(),
    staleSourceRef
  );

  assert.equal(refreshed.length, 1);
  assert.equal(refreshed[0].actorId, 'actor-transfer');
  assert.equal(String(refreshed[0].nodeRid), 'actor-b');
  assert.equal(refreshed[0].objectGeneration, 1n);
  assert.deepEqual(packetTargets[0], {
    actorId: 'actor-transfer',
    target: {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-b',
      spotId: 'zone-sw',
      spotKind: framework.ZLinkSpotKind.User
    }
  });
  assert.deepEqual(rebound, [targetRef]);
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(bytesOf(stream.writes[0]));
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { marker: 'after-transfer' });
});

test('same actor route update preserves the active session while routed traffic changes the Spot target', async () => {
  const actorRef = {
    nodeRid: 'actor-a', actorId: 'actor-local-move', generation: 1n,
    ownershipGeneration: 1n, ownerLeaseGeneration: 3n,
    bindingGeneration: 7n, acceptedHighWater: 9n
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-local-move', 'session-a'));
  const boundActor = await context.actors.bind(actorRef);
  let packetTarget;
  host.boundSessionRelay.actorPackets.updateRemoteActorPacketTarget = (_actorId, target) => {
    packetTarget = target;
  };

  const seal = serviceSessionRelocationSeal(actorRef.actorId, {
    actorGeneration: 1n,
    sourceNodeRid: 'actor-a',
    authorityOwnerGeneration: 1n,
    ownerLeaseGeneration: 3n,
    sessionRid: 'session-a',
    bindingGeneration: 7n
  });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(
    serviceSessionRelocationRoute(seal, {
      targetNodeRid: 'actor-a',
      targetAuthorityOwnerGeneration: 2n
    })
  );
  await host.boundSessionRelay.boundSessions.receiveRoutedBoundSession(
    actorRef.actorId,
    { marker: 'same-node-route-refresh' },
    'Notify',
    new Map(),
    (await host.streamBindingRuntime.find(actorRef.actorId)).ref,
    {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-a',
      spotId: 'zone-sw',
      spotKind: framework.ZLinkSpotKind.User
    }
  );

  assert.equal(await host.streamBindingRuntime.find(actorRef.actorId), boundActor);
  assert.equal(String(boundActor.ref.nodeRid), 'actor-a');
  assert.equal(packetTarget.spotId, 'zone-sw');
});

test('internal route refresh preserves object generation while explicit bind can replace an incarnation', async () => {
  const bindingRuntime = new framework.ZLinkStreamBindingRuntime();
  const context = bindingRuntime.createSessionContext(
    recordingStream('session-generation-fence', 'session-a')
  );
  const original = await context.actors.bind({
    nodeRid: 'actor-a',
    actorId: 'actor-generation-fence',
    objectGeneration: 1n,
    meshName: 'session-test'
  });

  await assert.rejects(
    bindingRuntime.refreshActor({
      nodeRid: 'actor-b',
      actorId: 'actor-generation-fence',
      objectGeneration: 2n,
      meshName: 'session-test'
    }),
    /cannot replace object generation/
  );
  assert.equal(await bindingRuntime.find('actor-generation-fence'), original);

  const replacement = await context.actors.bind({
    nodeRid: 'actor-b',
    actorId: 'actor-generation-fence',
    objectGeneration: 2n,
    meshName: 'session-test'
  });
  assert.notEqual(replacement, original);
  assert.equal(replacement.ref.objectGeneration, 2n);
});

test('one-way transferred actor route handler settles only after the local route replacement', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-transfer-ack', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-a', actorId: 'actor-transfer-ack', generation: 1n,
    ownershipGeneration: 1n, ownerLeaseGeneration: 3n,
    bindingGeneration: 7n, acceptedHighWater: 9n
  });
  let releaseRefresh;
  const refreshBlocked = new Promise((resolve) => { releaseRefresh = resolve; });
  const commitActorRoute = host.streamBindingRuntime.commitActorRoute.bind(
    host.streamBindingRuntime
  );
  host.streamBindingRuntime.commitActorRoute = async (...args) => {
    await refreshBlocked;
    await commitActorRoute(...args);
  };

  const seal = serviceSessionRelocationSeal('actor-transfer-ack', {
    actorGeneration: 1n,
    sourceNodeRid: 'actor-a',
    authorityOwnerGeneration: 1n,
    ownerLeaseGeneration: 3n,
    sessionRid: 'session-a',
    bindingGeneration: 7n
  });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);

  let settled = false;
  const pending = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(serviceSessionRelocationRoute(seal, {
      targetNodeRid: 'actor-b',
      targetAuthorityOwnerGeneration: 2n
    })).then((result) => {
    settled = true;
    return result;
  });

  await Promise.resolve();
  assert.equal(settled, false);

  releaseRefresh();
  assert.equal(await pending, undefined);
  assert.equal(
    String((await host.streamBindingRuntime.find('actor-transfer-ack')).ref.nodeRid),
    'actor-b'
  );
});

test('command 42 seal is exact and idempotent while command 43 echoes its immutable fence', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-seal', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-seal', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n, acceptedHighWater: 29n
  });

  const seal = serviceSessionRelocationSeal('actor-seal', {
    actorGeneration: 7n,
    sourceNodeRid: 'actor-source',
    authorityOwnerGeneration: 10n,
    ownerLeaseGeneration: 20n,
    sessionRid: 'session-a',
    bindingGeneration: 17n
  });
  const first = await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  const retry = await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  assert.deepEqual(first, {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    actor: seal.actor,
    session: seal.session
  });
  assert.deepEqual(retry, first);
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal({
      ...seal,
      actor: { ...seal.actor, authorityOwnerGeneration: 11n }
    }),
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );

  const unknownAbort = serviceSessionRelocationRoute({
    ...seal,
    relocation: { high: 7n, low: 10n }
  }, { action: 'abort' });
  await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(unknownAbort);
  assert.equal(
    await host.streamBindingRuntime.validateActorRouteSeal('actor-seal', serviceSessionSealKey(seal)),
    true
  );

  const abort = serviceSessionRelocationRoute(seal, { action: 'abort' });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(abort);
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(abort);
  assert.equal(
    await host.streamBindingRuntime.validateActorRouteSeal('actor-seal', serviceSessionSealKey(seal)),
    false
  );

  const nextSeal = { ...seal, relocation: { high: 7n, low: 11n } };
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(nextSeal);
  assert.equal(
    await host.streamBindingRuntime.validateActorRouteSeal(
      'actor-seal',
      serviceSessionSealKey(nextSeal)
    ),
    true
  );
});

test('M1 actorJoin command 42 waits for every pre-seal accepted Actor frame to finish', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  let releaseRelay;
  const relayCanFinish = new Promise((resolve) => { releaseRelay = resolve; });
  let relayStarted;
  const relayDidStart = new Promise((resolve) => { relayStarted = resolve; });
  host.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    async relay() {
      relayStarted();
      await relayCanFinish;
      return true;
    }
  });
  const context = host.streamBindingRuntime.createSessionContext(
    fakeStream('session-active-frame', 'session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-active-frame', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });

  context.enterDispatch(serviceRelayDispatchHeader('AcceptedBeforeSeal'));
  const relaying = actor.relay(serviceRelayMessage('{"accepted":true}'));
  await relayDidStart;

  let sealSettled = false;
  const sealing = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(serviceSessionRelocationSeal(actor.actorId))
    .then((value) => {
      sealSettled = true;
      return value;
    });
  try {
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(
      sealSettled,
      false,
      'command 42 cannot confirm the seal while its accepted frame is still active'
    );
  } finally {
    releaseRelay();
    await relaying;
    context.exitDispatch();
  }

  const sealed = await sealing;
  assert.deepEqual(sealed, {
    relocation: serviceSessionRelocationSeal(actor.actorId).relocation,
    coordinator: serviceSessionRelocationSeal(actor.actorId).coordinator,
    actor: serviceSessionRelocationSeal(actor.actorId).actor,
    session: serviceSessionRelocationSeal(actor.actorId).session
  });
});

test('route replacement shares the pre-bind active-frame drain with command 42', async () => {
  const registry = new ZLinkActorSessionBindingRegistry(16, 16, 50);
  const actorId = 'actor-replaced-active-frame';
  const bindingToken = 'binding-replaced-active-frame';
  const oldContext = { routingId: 'old-session', bindLocal() {}, unbindLocal() {} };
  const newContext = { routingId: 'new-session', bindLocal() {}, unbindLocal() {} };
  const oldActor = {
    actorId,
    ref: { actorId, objectGeneration: 5n, nodeRid: 'source', bindingGeneration: 6n }
  };
  await registry.bind(oldContext, oldActor, bindingToken);

  let releaseFrame;
  const frame = registry.runAcceptedFrameWhenReady(actorId, bindingToken, async () =>
    new Promise((resolve) => { releaseFrame = resolve; })
  );
  await new Promise((resolve) => setImmediate(resolve));
  const previous = await registry.requireRoute(actorId);
  await registry.replace(previous, newContext, {
    actorId,
    ref: { ...oldActor.ref, bindingGeneration: 7n }
  }, bindingToken, undefined, 'new-session');

  let sealed = false;
  const sealing = registry.sealAndWait(actorId, 'replacement-seal', {
    objectGeneration: 5n,
    bindingGeneration: 7n
  }).then(() => { sealed = true; });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(sealed, false, 'replacement must retain the still-active previous frame');

  releaseFrame();
  await frame;
  await sealing;
  assert.equal(sealed, true, 'previous frame completion must drain the replacement route');
  assert.equal(await registry.abortSeal(actorId, 'replacement-seal'), true);
});

test('captured Session REQUEST claims its submission before awaiting command 44', async () => {
  const registry = new ZLinkActorSessionBindingRegistry(16, 16, 50);
  const actorId = 'actor-request-synchronous-claim';
  const bindingToken = 'binding-request-synchronous-claim';
  const context = { routingId: 'session', bindLocal() {}, unbindLocal() {} };
  const actor = {
    actorId,
    ref: { actorId, objectGeneration: 5n, nodeRid: 'source', bindingGeneration: 6n }
  };
  await registry.bind(context, actor, bindingToken);

  const admission = await registry.beginAcceptedRequestFrameWhenReady(actorId, bindingToken);
  await registry.sealAndWait(actorId, 'request-synchronous-claim-seal', {
    objectGeneration: 5n,
    bindingGeneration: 6n
  });
  const first = admission.beginSubmission();
  assert.ok(first instanceof Promise);
  assert.equal(
    await admission.beginSubmission(),
    undefined,
    'an await boundary cannot leave the one-shot submission claim open'
  );

  await registry.abortSeal(actorId, 'request-synchronous-claim-seal');
  await first;
  await admission.complete();
});

test('seal captures an in-flight Session REQUEST frame and its detached terminal arrives after cutover once', async () => {
  const registry = new ZLinkActorSessionBindingRegistry(16, 16, 50);
  const lifecycle = new ZLinkActorSessionLifecycleCoordinator();
  const actorId = 'actor-request-relocation-admission';
  const bindingToken = 'binding-request-relocation-admission';
  const context = {
    routingId: 'session',
    dispatchHeader: {
      kind: streamProtocol.ZLinkStreamMessageKind.Request,
      codec: streamProtocol.ZLinkStreamCodec.Json,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      name: 'PlaceMarkReq',
      requestSeq: 2n,
      metadata: new Map()
    },
    bindLocal() {},
    unbindLocal() {}
  };
  const actor = {
    actorId,
    bindingToken,
    ref: { actorId, objectGeneration: 5n, nodeRid: 'source', bindingGeneration: 6n }
  };
  await registry.bind(context, actor, bindingToken);

  let relayStarted;
  const relayDidStart = new Promise((resolve) => { relayStarted = resolve; });
  let finishRelay;
  const relayCanFinish = new Promise((resolve) => { finishRelay = resolve; });
  let relaySubmissions = 0;
  let detachedTerminals = 0;
  let relaySettled = false;
  let cutoverPublished = false;
  const sender = new ZLinkBoundActorRelaySender(
    registry,
    {},
    {
      async relay(_actor, header) {
        relaySubmissions += 1;
        assert.equal(header.name, 'PlaceMarkReq');
        assert.equal(header.requestSeq, 2n, 'the original request correlation is preserved');
        relayStarted();
        await relayCanFinish;
        assert.equal(cutoverPublished, true, 'the detached terminal is delivered after cutover');
        detachedTerminals += 1;
        return true;
      }
    },
    lifecycle
  );
  const payload = framework.ZLinkMessage.fromEncoded(
    framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"position":4}'))
  );
  const relaying = sender.relay(actor, payload).then((result) => {
    relaySettled = true;
    return result;
  });
  await relayDidStart;

  await registry.sealAndWait(actorId, 'request-race-seal', {
    objectGeneration: 5n,
    bindingGeneration: 6n
  });
  assert.equal(
    relaySettled,
    false,
    'command 42 captures the Session frame without waiting for the submitted REQUEST terminal'
  );
  assert.equal(relaySubmissions, 1, 'capture cannot resubmit an already-started REQUEST');

  const previous = await registry.requireRoute(actorId);
  const targetContext = {
    ...context,
    routingId: 'session'
  };
  await lifecycle.run(actorId, async () => {
    await registry.replaceAndReleaseSeal(
      previous,
      targetContext,
      {
        ...actor,
        ref: { ...actor.ref, nodeRid: 'target' }
      },
      bindingToken,
      'request-race-seal',
      undefined,
      'session'
    );
    cutoverPublished = true;
  });
  assert.equal(cutoverPublished, true);

  finishRelay();
  await relaying;
  assert.equal(relaySubmissions, 1);
  assert.equal(cutoverPublished, true, 'the original REQUEST completes only after command 44');
  assert.equal(detachedTerminals, 1, 'the preserved request correlation has one terminal');
});

test('seal journals a captured REQUEST that has not submitted and replays it on the command 44 route once', async () => {
  const registry = new ZLinkActorSessionBindingRegistry(16, 16, 50);
  const lifecycle = new ZLinkActorSessionLifecycleCoordinator();
  const actorId = 'actor-request-captured-before-submit';
  const bindingToken = 'binding-request-captured-before-submit';
  const context = {
    routingId: 'session',
    dispatchHeader: {
      kind: streamProtocol.ZLinkStreamMessageKind.Request,
      codec: streamProtocol.ZLinkStreamCodec.Json,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      name: 'PlaceMarkReq',
      requestSeq: 3n,
      metadata: new Map()
    },
    bindLocal() {},
    unbindLocal() {}
  };
  const actor = {
    actorId,
    bindingToken,
    ref: { actorId, objectGeneration: 5n, nodeRid: 'source', bindingGeneration: 6n }
  };
  await registry.bind(context, actor, bindingToken);

  let releaseLane;
  const laneCanFinish = new Promise((resolve) => { releaseLane = resolve; });
  let laneStarted;
  const laneDidStart = new Promise((resolve) => { laneStarted = resolve; });
  const blockingTurn = lifecycle.run(actorId, async () => {
    laneStarted();
    await laneCanFinish;
  });
  await laneDidStart;

  const submittedRoutes = [];
  const sender = new ZLinkBoundActorRelaySender(
    registry,
    {},
    {
      async relay() {
        submittedRoutes.push((await registry.requireRoute(actorId)).actor.ref.nodeRid);
        return true;
      }
    },
    lifecycle
  );
  const relaying = sender.relay(actor, framework.ZLinkMessage.fromEncoded(
    framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"position":5}'))
  ));
  await new Promise((resolve) => setImmediate(resolve));

  await registry.sealAndWait(actorId, 'request-before-submit-seal', {
    objectGeneration: 5n,
    bindingGeneration: 6n
  });
  assert.deepEqual(submittedRoutes, [], 'capture before submit must not execute on the source route');

  releaseLane();
  await blockingTurn;
  await new Promise((resolve) => setImmediate(resolve));
  assert.deepEqual(submittedRoutes, [], 'the journaled REQUEST remains held until command 44');

  const previous = await registry.requireRoute(actorId);
  await lifecycle.run(actorId, async () => registry.replaceAndReleaseSeal(
    previous,
    context,
    { ...actor, ref: { ...actor.ref, nodeRid: 'target' } },
    bindingToken,
    'request-before-submit-seal',
    undefined,
    'session'
  ));
  await relaying;
  assert.deepEqual(submittedRoutes, ['target'], 'held replay submits once on the published target route');
});

test('one-way remote Actor relay releases its Session frame before a deferred Join terminal', async () => {
  const registry = new ZLinkActorSessionBindingRegistry(16, 16, 100);
  const actorId = 'actor-deferred-join-relay';
  const bindingToken = 'binding-deferred-join-relay';
  const context = { routingId: 'session', bindLocal() {}, unbindLocal() {} };
  const actor = {
    actorId,
    ref: { actorId, objectGeneration: 5n, nodeRid: 'source', bindingGeneration: 6n }
  };
  await registry.bind(context, actor, bindingToken);

  let handlerStarted;
  const handlerDidStart = new Promise((resolve) => { handlerStarted = resolve; });
  let releaseHandler;
  const handlerCanFinish = new Promise((resolve) => { releaseHandler = resolve; });
  let handlerFinished = false;
  let detachedDispatch;
  const failures = [];
  const relay = new ZLinkActorPacketRelay({
    routeTransport: {},
    streamBindingRuntime: () => ({ find() {} }),
    meshRouters: {
      defaultSpotRouterChannelId() { return undefined; },
      defaultRouterChannelId() { return undefined; }
    },
    actorManager: () => ({
      getState() { return { spotId: 'game-room' }; }
    }),
    spotManager: () => ({
      async dispatchRoutedActorPacket() {
        handlerStarted();
        await handlerCanFinish;
        handlerFinished = true;
      }
    }),
    spotNodeRuntime: () => undefined,
    detachedTaskRunner: {
      runDetached(_taskName, callback) {
        detachedDispatch = callback();
      }
    },
    errorSink: () => ({
      reportRuntimeTaskException(_taskName, error) { failures.push(error); }
    })
  });
  const payload = actorPacketWire.encodeRemoteActorPacketRelayPayload({
    actorId,
    header: streamProtocol.encodeStreamHeader({
      kind: streamProtocol.ZLinkStreamMessageKind.Send,
      codec: streamProtocol.ZLinkStreamCodec.Json,
      flags: streamProtocol.ZLinkStreamHeaderFlags.None,
      name: 'JoinGameMsg',
      metadata: new Map()
    }),
    payload: Buffer.from('{"roomId":"room-a"}')
  });

  const frame = registry.runAcceptedFrameWhenReady(actorId, bindingToken, async () => {
    const accepted = await relay.receiveRemoteActorPacketRelay(payload, {
      meshName: 'tictactoe',
      sourceNodeRid: 'session-owner'
    });
    assert.equal(accepted.ok, true, JSON.stringify(accepted));
  });
  await frame;
  await handlerDidStart;
  assert.equal(handlerFinished, false, 'deferred Join terminal still owns the Actor FIFO turn');

  await registry.sealAndWait(actorId, 'deferred-join-seal', {
    objectGeneration: 5n,
    bindingGeneration: 6n
  });
  assert.equal(await registry.abortSeal(actorId, 'deferred-join-seal'), true);

  releaseHandler();
  await detachedDispatch;
  assert.equal(handlerFinished, true);
  assert.deepEqual(failures, []);
});

test('Session relocation seal timeout uses the configured Location option', async () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.configureLocations().sessionRelocationSealTimeoutMs(10);
    })
  );
  const host = new framework.ZLinkFrameworkRuntimeHost({ registration });
  let releaseFrame;
  host.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    async relay() {
      await new Promise((resolve) => { releaseFrame = resolve; });
      return true;
    },
    sessionRelocationSealTimeoutMs: 10
  });
  const context = host.streamBindingRuntime.createSessionContext(
    fakeStream('session-configured-seal-timeout', 'session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-configured-seal-timeout', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const disconnected = [];
  host.streamBindingRuntime.disconnectBoundSession = async (actorId) => {
    disconnected.push(actorId);
  };

  context.enterDispatch(serviceRelayDispatchHeader('FrameHeldAcrossSeal'));
  const frame = actor.relay(serviceRelayMessage('{"held":true}'));
  await waitForCondition(() => releaseFrame !== undefined, 'active Session frame');
  const keepAlive = setTimeout(() => {}, 1_000);
  try {
    await assert.rejects(
      host.boundSessionRelay.boundSessions
        .receiveServiceWireSessionRelocationSeal(serviceSessionRelocationSeal(actor.actorId)),
      error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
    );
  } finally {
    clearTimeout(keepAlive);
  }
  assert.deepEqual(disconnected, [actor.actorId]);
  releaseFrame();
  await frame;
  context.exitDispatch();
});

test('command 42 sender deadline terminates even while RouteMesh submit is pending', async () => {
  const registration = framework.createFrameworkRegistration(
    framework.createFrameworkOptions((builder) => {
      builder.configureLocations().sessionRelocationSealTimeoutMs(10);
    })
  );
  const relocationRuntime = new ZLinkHostServiceRelocationRuntime({
    registration,
    locationStore: () => undefined,
    liveDescriptors: async () => [],
    currentOwner: () => undefined,
    meshNode: () => ({
      sendToNode: async () => await new Promise(() => {})
    })
  });

  const keepAlive = setTimeout(() => {}, 1_000);
  try {
    await assert.rejects(
      relocationRuntime.requestSessionRelocationSeal(
        'play.route',
        'session-owner',
        serviceSessionRelocationSeal('actor-pending-command-42')
      ),
      error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
    );
  } finally {
    clearTimeout(keepAlive);
  }
  await relocationRuntime.dispose();
});

test('service-wire command 42 holds post-seal ingress and matching abort releases only its waiter', async () => {
  const socket = new FakeStreamSocket();
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-service-abort', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const seal = serviceSessionRelocationSeal('actor-service-abort');
  const first = await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  const retry = await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  assert.deepEqual(retry, first);
  assert.deepEqual(first.actor, seal.actor);
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal({
      ...seal,
      actor: { ...seal.actor, authorityOwnerGeneration: 12n }
    }),
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );

  context.enterDispatch(serviceRelayDispatchHeader('HeldAfterSeal'));
  try {
    const relaying = actor.relay(serviceRelayMessage('{"held":true}'));
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(socket.boundActorSends.length, 0);

    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute({
        relocation: { ...seal.relocation, low: 10n },
        coordinator: seal.coordinator,
        senderRole: 'source',
        actor: seal.actor.actor,
        session: seal.session,
        route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
      });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(socket.boundActorSends.length, 0);

    const abort = {
      relocation: seal.relocation,
      coordinator: seal.coordinator,
      senderRole: 'source',
      actor: seal.actor.actor,
      session: seal.session,
      route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
    };
    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute(abort);
    await relaying;
    assert.equal(socket.boundActorSends.length, 1);
    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute(abort);
    assert.equal(socket.boundActorSends.length, 1);
  } finally {
    context.exitDispatch();
  }
});

test('one-way service-wire command 44 atomically switches the route and releases held ingress', async () => {
  const socket = new FakeStreamSocket();
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-service-commit', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const seal = serviceSessionRelocationSeal('actor-service-commit');
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  const commit = serviceSessionRelocationRoute(seal);

  context.enterDispatch(serviceRelayDispatchHeader('HeldUntilCommit'));
  try {
    const relaying = actor.relay(serviceRelayMessage('{"commit":true}'));
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(socket.boundActorSends.length, 0);
    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute(commit);
    assert.equal(String((await host.streamBindingRuntime.find(actor.actorId)).ref.nodeRid), 'target');
    await relaying;
    assert.equal(socket.boundActorSends.length, 1);

    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute(commit);
    assert.equal(socket.boundActorSends.length, 1);
    await assert.rejects(
      host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute({
        ...commit,
        route: { ...commit.route, targetNodeGeneration: 5n }
      }),
      error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
    );
  } finally {
    context.exitDispatch();
  }
});

test('command 36 decodes the canonical actor route lease emitted by other runtimes', () => {
  // Canonical command 36 bytes for actor a@2, node n@3, authority 4,
  // owner lease 5, and binding 6. This literal is intentionally independent
  // of the Node encoder so an omitted actor-route field cannot self-validate.
  const canonical = Buffer.from(
    '5a4d012400'
    + '0161' + '0000000000000002'
    + '016e' + '0000000000000003'
    + '0000000000000004'
    + '0000000000000005'
    + '0000000000000006',
    'hex'
  );

  assert.deepEqual(serviceStatefulWire.decodeStatefulHeader(canonical), {
    kind: 'boundSessionSend',
    actor: {
      actor: { actorId: 'a', generation: 2n, nodeRid: 'n' },
      targetNodeGeneration: 3n,
      authorityOwnerGeneration: 4n,
      ownerLeaseGeneration: 5n
    },
    expectedBindingGeneration: 6n
  });
  assert.equal(serviceStatefulWire.encodeBoundSessionSendHeader({
    actor: { actorId: 'a', generation: 2n, nodeRid: 'n' },
    targetNodeGeneration: 3n,
    authorityOwnerGeneration: 4n,
    ownerLeaseGeneration: 5n
  }, 6n).toString('hex'), canonical.toString('hex'));
});

test('production command 36 preserves its producer fence without duplicating Location authority validation', async () => {
  const actorId = 'actor-command36-sender-lease';
  const wireHeaders = [];
  const ingressResults = [];
  let serviceIngress;
  const raw = {
    setServiceIngress(handler) {
      serviceIngress = handler;
    },
    async sendService(targetNodeRid, parts) {
      assert.equal(targetNodeRid, 'session-owner');
      wireHeaders.push(serviceStatefulWire.decodeStatefulHeader(parts[0]));
      const result = await serviceIngress({
        command: serviceStatefulWire.M6bServiceWireCommand.boundSessionSend,
        flags: 0,
        sourceRoutingId: 'actor-node',
        sourceNodeGeneration: 4n,
        parts
      });
      ingressResults.push(result);
      return result === 'application';
    }
  };
  const serviceRuntime = new ServiceStatefulRuntime(raw, 'actor-node', 4n);
  const serviceActor = serviceRuntime.restoreActorAuthority(
    actorId,
    'actor',
    5n,
    11n,
    'actor-node',
    4n,
    1n
  );
  const aggregateHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = aggregateHost.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(
      new FakeStreamSocket(),
      'session',
      undefined,
      undefined,
      undefined,
      'public-session'
    )
  );
  await context.actors.bind({
    nodeRid: 'actor-node',
    actorId,
    generation: 5n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 4n,
    bindingGeneration: 1n,
    acceptedHighWater: 0n
  });
  const aggregate = actorSessionBindingRuntimeOwner(aggregateHost.streamBindingRuntime);
  let deliveries = 0;
  const bindingResult = await serviceRuntime.bindSession(
    'session',
    serviceActor.ref,
    1000,
    () => {
      deliveries += 1;
      return true;
    },
    undefined,
    {
      retainOutbound: (claim, delivery) => aggregate.admitRelocationOutbound(claim, delivery),
      clearOutbound: (retainedActorId, error) => aggregate.clearRelocation(retainedActorId, error)
    }
  ).promise;
  assert.equal(bindingResult.terminalResult, RequestResult.Ok);
  const binding = serviceRuntime.sessionBindings('session')[0];
  assert.ok(binding);
  serviceRuntime.restoreActorSessionBinding(
    serviceActor.ref,
    'session-owner',
    'session',
    binding.bindingGeneration
  );

  const serviceSubmitResults = [];
  const senderHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  senderHost.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({
        routingId: zlink.RoutingId.from('actor-node'),
        lifecycleGeneration: 4n
      }),
      async sendActorBoundSession(actor, bindingGeneration, _parts, _flags, actorFence) {
        const result = await serviceRuntime.sendBoundSession(
          actor,
          bindingGeneration,
          {
            packetName: 'RelocationNotice',
            contentType: 'application/json',
            payload: Buffer.from('{"accepted":true}')
          },
          actorFence
        );
        serviceSubmitResults.push(result);
        return result;
      },
      async closeActorBoundSession() {}
    }
  };
  senderHost.setActorManager({
    getState(requestedActorId) {
      return requestedActorId === actorId
        ? {
            actor: { actorId },
            nativeActorRef: serviceActor.ref,
            boundSessionBindingGeneration: binding.bindingGeneration,
            locationGeneration: 11n,
            ownerLeaseGeneration: 13n
          }
        : undefined;
    }
  });

  await senderHost.createActorManagerOptions()
    .boundSessionFactory(actorId)
    .send({ accepted: true })
    .packetName('RelocationNotice')
    .submit();

  assert.equal(wireHeaders.length, 1);
  assert.equal(wireHeaders[0].actor.targetNodeGeneration, 4n);
  assert.equal(wireHeaders[0].actor.authorityOwnerGeneration, 11n);
  assert.equal(wireHeaders[0].actor.ownerLeaseGeneration, 13n);
  assert.deepEqual(serviceSubmitResults, [SubmitResult.Ok]);
  assert.deepEqual(ingressResults, ['application']);
  assert.equal(deliveries, 1);

  const wrongLeaseResult = await serviceRuntime.sendBoundSession(
    serviceActor.ref,
    binding.bindingGeneration,
    {
      packetName: 'RelocationNotice',
      contentType: 'application/json',
      payload: Buffer.from('{"wrongLease":true}')
    },
    {
      targetNodeGeneration: 4n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 14n
    }
  );
  assert.equal(wrongLeaseResult, SubmitResult.Ok);
  assert.equal(ingressResults.at(-1), 'application');
  assert.equal(deliveries, 2);
  serviceRuntime.close();
});

test('M1 actorJoin actual command 36 stays FIFO-held until exact atomic route apply', async () => {
  let serviceIngress;
  const serviceRuntime = new ServiceStatefulRuntime({
    setServiceIngress(handler) {
      serviceIngress = handler;
    }
  }, 'session-owner', 4n);
  const serviceActor = serviceRuntime.restoreActorAuthority(
    'actor-fifo-terminal',
    'actor',
    5n,
    11n,
    'session-owner',
    4n,
    1n
  );
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(new FakeStreamSocket(), 'session', 'public-session')
  );
  await context.actors.bind({
    nodeRid: 'session-owner', actorId: 'actor-fifo-terminal', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 4n, bindingGeneration: 1n, acceptedHighWater: 41n
  });
  const sealTemplate = serviceSessionRelocationSeal('actor-fifo-terminal');
  const seal = {
    ...sealTemplate,
    actor: {
      ...sealTemplate.actor,
      actor: { ...sealTemplate.actor.actor, nodeRid: 'session-owner' },
      targetNodeGeneration: 4n
    },
    session: {
      ...sealTemplate.session,
      sessionOwnerId: 'session-owner',
      sessionOwnerLeaseGeneration: 4n,
      bindingGeneration: 1n
    }
  };
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  const aggregate = actorSessionBindingRuntimeOwner(host.streamBindingRuntime);
  const delivered = [];
  const bindingResult = await serviceRuntime.bindSession(
    'session',
    serviceActor.ref,
    1000,
    (_sessionRid, payloadFrame) => {
      delivered.push(JSON.parse(
        serviceWire.decodeApplicationPayload(payloadFrame).payload.toString('utf8')
      ).order);
      return true;
    },
    undefined,
    {
      retainOutbound: (claim, delivery) =>
        aggregate.admitRelocationOutbound(claim, delivery),
      clearOutbound: (actorId, error) =>
        aggregate.clearRelocation(actorId, error)
    }
  ).promise;
  assert.equal(bindingResult.terminalResult, RequestResult.Ok);
  const binding = serviceRuntime.sessionBindings('session')[0];
  assert.ok(binding);
  const command36 = async (order, actorFence = {
    actor: serviceActor.ref,
    targetNodeGeneration: 4n,
    authorityOwnerGeneration: serviceActor.authorityOwnerGeneration,
    ownerLeaseGeneration: 13n
  }, expectedBindingGeneration = binding.bindingGeneration, sourceRoutingId = actorFence.actor.nodeRid) => serviceIngress({
    command: serviceStatefulWire.M6bServiceWireCommand.boundSessionSend,
    flags: 0,
    sourceRoutingId,
    sourceNodeGeneration: actorFence.targetNodeGeneration,
    parts: [
      serviceStatefulWire.encodeBoundSessionSendHeader(
        actorFence,
        expectedBindingGeneration
      ),
      serviceWire.encodeApplicationPayload({
        packetName: 'RelocationNotice',
        contentType: 'application/json',
        payload: Buffer.from(JSON.stringify({ order }))
      })
    ]
  });
  assert.equal(await command36(0, {
    actor: serviceActor.ref,
    targetNodeGeneration: 4n,
    authorityOwnerGeneration: serviceActor.authorityOwnerGeneration + 1n,
    ownerLeaseGeneration: 13n
  }), 'application');
  assert.equal(await command36(0, {
    actor: serviceActor.ref,
    targetNodeGeneration: 5n,
    authorityOwnerGeneration: serviceActor.authorityOwnerGeneration,
    ownerLeaseGeneration: 13n
  }), 'application');
  assert.equal(await command36(0, {
    actor: serviceActor.ref,
    targetNodeGeneration: 4n,
    authorityOwnerGeneration: serviceActor.authorityOwnerGeneration,
    ownerLeaseGeneration: 14n
  }), 'application');
  assert.equal(
    await command36(0, undefined, binding.bindingGeneration + 1n),
    'protocolError'
  );
  assert.equal(await command36(1), 'application');
  assert.equal(await command36(2), 'application');
  const targetFence = {
    actor: { ...serviceActor.ref, nodeRid: 'target' },
    targetNodeGeneration: 4n,
    authorityOwnerGeneration: 12n,
    ownerLeaseGeneration: 14n
  };
  assert.equal(await command36(3, targetFence), 'application');
  assert.equal(await command36(3, targetFence), 'application');
  assert.deepEqual(delivered, []);

  const commit = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });
  await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute({
      ...commit,
      relocation: { ...commit.relocation, low: commit.relocation.low + 1n }
    });
  assert.deepEqual(delivered, []);

  await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  await waitForCondition(() => delivered.length === 7, 'actual command 36 FIFO drain');
  assert.deepEqual(delivered, [0, 0, 0, 1, 2, 3, 3]);
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute({
      ...commit,
      route: { ...commit.route, targetNodeGeneration: 5n }
    }),
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );
  await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  assert.equal(await command36(4), 'protocolError');
  assert.equal(await command36(4, targetFence, binding.bindingGeneration, 'session-owner'), 'protocolError');
  assert.deepEqual(delivered, [0, 0, 0, 1, 2, 3, 3]);
  assert.equal(await command36(4, targetFence), 'application');
  assert.deepEqual(delivered, [0, 0, 0, 1, 2, 3, 3, 4]);
  assert.equal(await command36(5, {
    ...targetFence,
    ownerLeaseGeneration: 15n
  }), 'application');
  assert.deepEqual(delivered, [0, 0, 0, 1, 2, 3, 3, 4, 5]);
  serviceRuntime.close();
});

test('actual raw command 36 binds successor header to the authenticated peer tenure before Store', async () => {
  const actorId = 'actor-command36-authenticated-source';
  const raw = new RawServiceMeshRuntime({
    descriptor: testServiceNodeDescriptor('session-owner', 4n),
    bindingPort: {},
    applicationJobQueue: new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration()
    )
  });
  assert.equal(raw.topology.admit(
    testServiceNodeDescriptor('source', 4n),
    'source-connection'
  ), 'admitted');
  assert.equal(raw.topology.admit(
    testServiceNodeDescriptor('target', 5n),
    'target-connection'
  ), 'admitted');
  const serviceRuntime = new ServiceStatefulRuntime(raw, 'session-owner', 4n);
  const serviceActor = serviceRuntime.restoreActorAuthority(
    actorId,
    'actor',
    5n,
    11n,
    'session-owner',
    4n,
    1n
  );
  const aggregate = await testActorRouteAggregate({
    actorId,
    objectGeneration: 5n,
    generation: 5n,
    nodeRid: 'target',
    meshName: 'play.route',
    ownershipGeneration: 12n,
    ownerLeaseGeneration: 14n,
    ownerNodeGeneration: 5n,
    bindingGeneration: 1n,
    acceptedHighWater: 0n
  }, 8, 2, 'session');
  let storeCalls = 0;
  let deliveries = 0;
  const bindingResult = await serviceRuntime.bindSession(
    'session',
    serviceActor.ref,
    1000,
    () => {
      deliveries += 1;
      return true;
    },
    undefined,
    {
      retainOutbound(claim, delivery) {
        storeCalls += 1;
        return aggregate.owner.admitRelocationOutbound(claim, delivery);
      },
      clearOutbound: (value, error) => aggregate.owner.clearRelocation(value, error)
    }
  ).promise;
  assert.equal(bindingResult.terminalResult, RequestResult.Ok);
  const binding = serviceRuntime.sessionBindings('session')[0];
  assert.ok(binding);
  const header = serviceStatefulWire.encodeBoundSessionSendHeader({
    actor: { ...serviceActor.ref, nodeRid: 'target' },
    targetNodeGeneration: 5n,
    authorityOwnerGeneration: 12n,
    ownerLeaseGeneration: 14n
  }, binding.bindingGeneration);
  const payload = serviceWire.encodeApplicationPayload({
    packetName: 'RelocationNotice',
    contentType: 'application/json',
    payload: Buffer.from('{"accepted":true}')
  });
  const ingress = sourceRid => raw.processReceived({
    sourceRid,
    sourceRoute: Buffer.from(sourceRid),
    parts: [header, payload]
  }, 0, false);

  assert.equal(await ingress('source'), 'protocolError');
  assert.equal(storeCalls, 0);
  assert.equal(deliveries, 0);
  assert.equal(await ingress('target'), 'application');
  assert.equal(storeCalls, 1);
  assert.equal(deliveries, 1);
  serviceRuntime.close();
  raw.close();
});

test('actual command 36 retains the exact physical target after off-wire ownership advances first', async () => {
  const actorId = 'actor-command36-off-wire-owner-advance';
  const sourceRef = {
    actorId,
    objectGeneration: 5n,
    generation: 5n,
    nodeRid: 'session-owner',
    meshName: 'play.route',
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 4n,
    bindingGeneration: 1n,
    acceptedHighWater: 0n
  };
  const aggregate = await testActorRouteAggregate(sourceRef, 8, 4, 'session');
  let serviceIngress;
  const serviceRuntime = new ServiceStatefulRuntime({
    setServiceIngress(handler) {
      serviceIngress = handler;
    }
  }, 'session-owner', 4n);
  const serviceActor = serviceRuntime.restoreActorAuthority(
    actorId,
    'actor',
    5n,
    11n,
    'session-owner',
    4n,
    1n
  );
  const delivered = [];
  const failed = [];
  const bindingResult = await serviceRuntime.bindSession(
    'session',
    serviceActor.ref,
    1000,
    (_sessionRid, payloadFrame) => {
      delivered.push(JSON.parse(
        serviceWire.decodeApplicationPayload(payloadFrame).payload.toString('utf8')
      ).order);
      return true;
    },
    undefined,
    {
      retainOutbound: (claim, delivery) => aggregate.owner.admitRelocationOutbound(claim, {
        deliver: () => delivery.deliver(),
        fail(error) {
          failed.push(error);
          delivery.fail(error);
        }
      }),
      clearOutbound: (value, error) => aggregate.owner.clearRelocation(value, error)
    }
  ).promise;
  assert.equal(bindingResult.terminalResult, RequestResult.Ok);
  const binding = serviceRuntime.sessionBindings('session')[0];
  assert.ok(binding);

  const seal = {
    actorId,
    actorGeneration: 5n,
    actorOwnershipGeneration: 11n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 13n,
    sessionIdentity: 'session',
    actorNodeRid: 'session-owner',
    actorNodeGeneration: 4n,
    sealId: 'off-wire-owner-advance-seal'
  };
  await aggregate.owner.sealRelocation(seal, {
    objectGeneration: 5n,
    authorityOwnerGeneration: 11n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 13n
  });
  const targetRef = {
    ...sourceRef,
    nodeRid: 'target',
    ownershipGeneration: 12n,
    ownerLeaseGeneration: 14n,
    ownerNodeGeneration: 5n
  };
  await aggregate.publish(targetRef);

  const command36 = async (order, actorNodeRid, actorNodeGeneration, authority, ownerLease) =>
    serviceIngress({
      command: serviceStatefulWire.M6bServiceWireCommand.boundSessionSend,
      flags: 0,
      sourceRoutingId: actorNodeRid,
      sourceNodeGeneration: actorNodeGeneration,
      parts: [
        serviceStatefulWire.encodeBoundSessionSendHeader({
          actor: { ...serviceActor.ref, nodeRid: actorNodeRid },
          targetNodeGeneration: actorNodeGeneration,
          authorityOwnerGeneration: authority,
          ownerLeaseGeneration: ownerLease
        }, binding.bindingGeneration),
        serviceWire.encodeApplicationPayload({
          packetName: 'RelocationNotice',
          contentType: 'application/json',
          payload: Buffer.from(JSON.stringify({ order }))
        })
      ]
    });

  assert.equal(await command36(1, 'target', 5n, 12n, 14n), 'application');
  assert.equal(await command36(2, 'target', 5n, 11n, 13n), 'application');
  assert.equal(await command36(3, 'target', 5n, 12n, 15n), 'application');
  assert.equal(await command36(4, 'other-target', 6n, 12n, 14n), 'application');
  assert.equal(await command36(5, 'future-target', 6n, 13n, 15n), 'protocolError');
  assert.deepEqual(delivered, []);

  await aggregate.owner.applyRelocation(
    actorId,
    seal.sealId,
    'off-wire-owner-advance-apply',
    'commit',
    async () => {
      assert.equal(await aggregate.port.abortActorRouteSeal(actorId, seal.sealId), true);
    },
    {
      actorId,
      objectGeneration: 5n,
      actorNodeRid: 'target',
      actorNodeGeneration: 5n,
      sessionIdentity: 'session',
      bindingGeneration: 1n
    }
  );
  await waitForCondition(() => delivered.length === 3, 'off-wire advanced target FIFO drain');
  assert.deepEqual(delivered, [1, 2, 3]);
  assert.equal(failed.length, 2);
  assert.match(String(failed[0]), /capacity/);
  assert.match(String(failed[1]), /did not match command 44 proof/);
  serviceRuntime.close();
});

test('actual command 36 FIFO bounds capacity and settles false throw and duplicate shutdown once', async () => {
  const actorId = 'actor-command36-settlement';
  const sourceRef = {
    actorId,
    objectGeneration: 5n,
    generation: 5n,
    nodeRid: 'session-owner',
    meshName: 'play.route',
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 4n,
    bindingGeneration: 1n,
    acceptedHighWater: 0n
  };
  const aggregate = await testActorRouteAggregate(sourceRef, 8, 2, 'session');
  let serviceIngress;
  const serviceRuntime = new ServiceStatefulRuntime({
    setServiceIngress(handler) {
      serviceIngress = handler;
    }
  }, 'session-owner', 4n);
  const serviceActor = serviceRuntime.restoreActorAuthority(
    actorId,
    'actor',
    5n,
    11n,
    'session-owner',
    4n,
    1n
  );
  const attempted = [];
  const failures = [];
  const bindingResult = await serviceRuntime.bindSession(
    'session',
    serviceActor.ref,
    1000,
    (_sessionRid, payloadFrame) => {
      const order = JSON.parse(
        serviceWire.decodeApplicationPayload(payloadFrame).payload.toString('utf8')
      ).order;
      attempted.push(order);
      if (order === 2) return false;
      if (order === 4) throw new Error('delivery-throw');
      return true;
    },
    undefined,
    {
      retainOutbound: (claim, delivery) => aggregate.owner.admitRelocationOutbound(
        claim,
        {
          deliver: () => delivery.deliver(),
          fail(error) {
            failures.push(error);
            delivery.fail(error);
          }
        }
      ),
      clearOutbound: (value, error) => aggregate.owner.clearRelocation(value, error)
    }
  ).promise;
  assert.equal(bindingResult.terminalResult, RequestResult.Ok);
  const binding = serviceRuntime.sessionBindings('session')[0];
  assert.equal(binding.bindingGeneration, 1n);
  const command36 = async (
    order,
    actor = serviceActor.ref,
    nodeGeneration = 4n,
    authority = 11n,
    ownerLease = authority === 11n ? 13n : 14n
  ) =>
    serviceIngress({
      command: serviceStatefulWire.M6bServiceWireCommand.boundSessionSend,
      flags: 0,
      sourceRoutingId: actor.nodeRid,
      sourceNodeGeneration: nodeGeneration,
      parts: [
        serviceStatefulWire.encodeBoundSessionSendHeader({
          actor,
          targetNodeGeneration: nodeGeneration,
          authorityOwnerGeneration: authority,
          ownerLeaseGeneration: ownerLease
        }, binding.bindingGeneration),
        serviceWire.encodeApplicationPayload({
          packetName: 'RelocationNotice',
          contentType: 'application/json',
          payload: Buffer.from(JSON.stringify({ order }))
        })
      ]
    });
  const sourceSeal = {
    actorId,
    actorGeneration: 5n,
    actorOwnershipGeneration: 11n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 13n,
    sessionIdentity: 'session',
    actorNodeRid: 'session-owner',
    actorNodeGeneration: 4n,
    sealId: 'command36-source-seal'
  };
  await aggregate.owner.sealRelocation(sourceSeal, {
    objectGeneration: 5n,
    authorityOwnerGeneration: 11n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 13n
  });
  assert.equal(await command36(1), 'application');
  assert.equal(await command36(2), 'application');
  assert.equal(await command36(3), 'protocolError');
  assert.deepEqual(attempted, []);
  assert.equal(failures.length, 1);
  assert.match(String(failures[0]), /capacity/);

  const targetRef = {
    ...sourceRef,
    nodeRid: 'target',
    ownershipGeneration: 12n,
    ownerLeaseGeneration: 14n,
    ownerNodeGeneration: 5n
  };
  await aggregate.owner.applyRelocation(
    actorId,
    sourceSeal.sealId,
    'command36-source-apply',
    'commit',
    async () => aggregate.publish(targetRef, {
      sealId: sourceSeal.sealId
    }),
    {
      actorId,
      objectGeneration: 5n,
      actorNodeRid: 'target',
      actorNodeGeneration: 5n,
      sessionIdentity: 'session',
      bindingGeneration: 1n
    }
  );
  await waitForCondition(() => attempted.length === 2, 'false delivery settlement');
  assert.deepEqual(attempted, [1, 2]);
  assert.equal(failures.length, 2);
  assert.match(String(failures[1]), /delivery was rejected/);
  await aggregate.owner.observeRelocationTerminal(
    actorId,
    sourceSeal.sealId,
    'command36-source-apply'
  );

  const targetActor = { ...serviceActor.ref, nodeRid: 'target' };
  const throwSeal = {
    ...sourceSeal,
    actorOwnershipGeneration: 12n,
    ownerLeaseGeneration: 14n,
    actorNodeRid: 'target',
    actorNodeGeneration: 5n,
    sealId: 'command36-throw-seal'
  };
  await aggregate.owner.sealRelocation(throwSeal, {
    objectGeneration: 5n,
    authorityOwnerGeneration: 12n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 14n
  });
  assert.equal(await command36(4, targetActor, 5n, 12n), 'application');
  assert.equal(await command36(5, targetActor, 5n, 12n), 'application');
  await aggregate.owner.applyRelocation(
    actorId,
    throwSeal.sealId,
    'command36-throw-apply',
    'abort',
    async () => {
      assert.equal(await aggregate.port.abortActorRouteSeal(actorId, throwSeal.sealId), true);
    }
  );
  await waitForCondition(() => attempted.length === 4, 'throw delivery settlement');
  assert.deepEqual(attempted, [1, 2, 4, 5]);
  assert.equal(failures.length, 3);
  assert.match(String(failures[2]), /delivery-throw/);
  await aggregate.owner.observeRelocationTerminal(
    actorId,
    throwSeal.sealId,
    'command36-throw-apply'
  );

  const shutdownSeal = { ...throwSeal, sealId: 'command36-shutdown-seal' };
  await aggregate.owner.sealRelocation(shutdownSeal, {
    objectGeneration: 5n,
    authorityOwnerGeneration: 12n,
    bindingGeneration: 1n,
    ownerLeaseGeneration: 14n
  });
  assert.equal(await command36(6, targetActor, 5n, 12n), 'application');
  assert.equal(await command36(7, targetActor, 5n, 12n), 'application');
  const failuresBeforeShutdown = failures.length;
  serviceRuntime.close();
  assert.equal(failures.length, failuresBeforeShutdown + 2);
  assert.equal(attempted.includes(6), false);
  assert.equal(attempted.includes(7), false);
  serviceRuntime.close();
  assert.equal(failures.length, failuresBeforeShutdown + 2);
});

test('per-actor relocation lineage and pending capacity stay bounded across repeated terminals', async () => {
  const actorId = 'actor-bounded-relocation-lineage';
  const aggregate = await testActorRouteAggregate({
    actorId,
    objectGeneration: 5n,
    generation: 5n,
    nodeRid: 'source',
    meshName: 'play.route',
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    ownerNodeGeneration: 4n,
    bindingGeneration: 1n,
    acceptedHighWater: 0n
  }, 2, 2, 'session');
  const delivered = [];
  const failed = [];
  const seals = [];

  for (let iteration = 1; iteration <= 3; iteration++) {
    const sealId = `bounded-lineage-${iteration}`;
    seals.push(sealId);
    const seal = {
      actorId,
      actorGeneration: 5n,
      actorOwnershipGeneration: 11n,
      bindingGeneration: 1n,
      ownerLeaseGeneration: 13n,
      sessionIdentity: 'session',
      actorNodeRid: 'source',
      actorNodeGeneration: 4n,
      sealId
    };
    await aggregate.owner.sealRelocation(seal, {
      objectGeneration: 5n,
      authorityOwnerGeneration: 11n,
      bindingGeneration: 1n,
      ownerLeaseGeneration: 13n
    });
    const retain = value => aggregate.owner.admitRelocationOutbound({
      actorId,
      objectGeneration: 5n,
      actorNodeRid: 'source',
      actorNodeGeneration: 4n,
      authorityOwnerGeneration: 11n,
      ownerLeaseGeneration: 13n,
      producerNodeRid: 'source',
      producerNodeGeneration: 4n,
      sessionIdentity: 'session',
      bindingGeneration: 1n
    }, {
      async deliver() {
        delivered.push(value);
        return true;
      },
      fail(error) {
        failed.push(error);
      }
    });
    assert.equal(await retain(`${iteration}.1`), 'retained');
    assert.equal(await retain(`${iteration}.2`), 'retained');
    assert.equal(await retain(`${iteration}.3`), 'rejected');
    await aggregate.owner.applyRelocation(
      actorId,
      sealId,
      `bounded-lineage-apply-${iteration}`,
      'abort',
      async () => {
        assert.equal(await aggregate.port.abortActorRouteSeal(actorId, sealId), true);
      }
    );
    await waitForCondition(
      () => delivered.length === iteration * 2,
      `bounded relocation lineage drain ${iteration}`
    );
    await aggregate.owner.observeRelocationTerminal(
      actorId,
      sealId,
      `bounded-lineage-apply-${iteration}`
    );
  }

  assert.equal(failed.length, 3);
  assert.ok(failed.every(error => /capacity/.test(String(error))));
  assert.equal(await aggregate.owner.relocationSnapshot(actorId, seals[0]), undefined);
  assert.ok(await aggregate.owner.relocationSnapshot(actorId, seals[1]));
  assert.ok(await aggregate.owner.relocationSnapshot(actorId, seals[2]));
  assert.deepEqual(delivered, ['1.1', '1.2', '2.1', '2.2', '3.1', '3.2']);
});

test('OnJoinedActor public bound-session push waits for route convergence and is delivered exactly once', async () => {
  const relocationBehavior = JSON.parse(fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/conformance/relocation-behavior-v1.json'),
    'utf8'
  ));
  const boundSessionBehavior = JSON.parse(fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/conformance/bound-session-relocation-v1.json'),
    'utf8'
  ));
  const behavior = relocationBehavior.trafficScenarios.find(
    value => value.name === 'target-lifecycle-callback-bound-session-push'
  );
  assert.ok(behavior, 'relocation behavior fixture must define the callback push scenario');
  assert.equal(boundSessionBehavior.trafficScenarios.actorJoinLifecycle, behavior.name);
  assert.equal(boundSessionBehavior.invariants.trafficSubmittedWhileSealedIsRetained, true);
  assert.equal(boundSessionBehavior.invariants.latePredecessorTerminalAffectsSuccessor, false);
  const checkpoints = new Map(behavior.checkpoints.map(value => [value.name, value]));

  const actorId = 'actor-lifecycle-callback-push';
  const socket = new FakeStreamSocket();
  const sessionHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const targetHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = sessionHost.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  const actorRef = {
    nodeRid: 'source', actorId, generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  };
  await context.actors.bind(actorRef);
  targetHost.setActorManager({
    getState(requestedActorId) {
      return requestedActorId === actorId
        ? {
            actor: { context: { actorId } },
            nativeActorRef: actorRef,
            remoteBoundSessionTarget: {
              routerChannelId: 'session.route',
              targetNodeRid: 'session-owner',
              spotId: 'session-entry'
            }
          }
        : undefined;
    }
  });
  let remoteSubmissions = 0;
  targetHost.routeTransport.submitInfrastructure = async (
    _routerChannelId,
    _targetNodeRid,
    packetName,
    payload
  ) => {
    assert.equal(packetName, '__zlink.actor.bound_session.send');
    remoteSubmissions += 1;
    return await sessionHost.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSend(payload);
  };

  const seal = serviceSessionRelocationSeal(actorId);
  await sessionHost.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);

  class CallbackSpot {
    async onActorJoin() {
      return { accepted: true };
    }
    async onJoinedActor() {
      await targetHost.createActorManagerOptions()
        .boundSessionFactory(actorId)
        .send({ event: 'joined' })
        .packetName('JoinedNotice')
        .submit();
    }
  }
  const callbackManager = new framework.DefaultZLinkSpotManager({
    spotFactories: [CallbackSpot],
    entrySpotCallbacks: { async onLeaveActor() {} }
  });
  await callbackManager.getOrCreate('callback.mesh', CallbackSpot, 'callback-spot');
  const callbackActor = {
    actorId,
    context: {
      actorId,
      [framework.ZLINK_ACTOR_LIFECYCLE_SNAPSHOT]() {
        return {
          actorRef: { nodeRid: 'target', actorId, generation: 5n },
          actorType: 'CallbackActor',
          membershipEpoch: 1n
        };
      }
    }
  };
  const joinRequest = zlink.Message.from('join');
  try {
    await callbackManager.admitActorJoin(
      'callback-spot',
      callbackActor,
      joinRequest,
      () => undefined
    );
  } finally {
    joinRequest.close();
  }
  await waitForCondition(
    () => remoteSubmissions === 1,
    'OnJoinedActor public bound-session push arrival at its Session owner'
  );
  assert.equal(
    socket.sends.length,
    checkpoints.get('afterCallbackSubmit').deliveryCount
  );
  assert.equal(
    socket.sends.length,
    checkpoints.get('beforeSessionRouteConverged').deliveryCount
  );
  assert.equal(
    socket.sends.length,
    boundSessionBehavior.invariants.deliveryBeforeRouteApply
  );

  const commit = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });
  await sessionHost.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  await waitForCondition(
    () => socket.sends.length === checkpoints.get('afterSessionRouteConverged').deliveryCount,
    'OnJoinedActor retained bound-session delivery after Session route convergence'
  );
  assert.equal(
    socket.sends.length,
    checkpoints.get('afterSessionRouteConverged').deliveryCount
  );
  assert.equal(
    boundSessionBehavior.invariants.routeApplyReleasesRetainedTrafficExactlyOnce,
    true
  );

  const deliveryBeforeDuplicate = socket.sends.length;
  await sessionHost.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  assert.equal(
    socket.sends.length,
    checkpoints.get('afterDuplicateRouteTerminal').deliveryCount
  );
  assert.equal(
    socket.sends.length - deliveryBeforeDuplicate,
    boundSessionBehavior.invariants.duplicateRouteTerminalAdditionalDelivery
  );
});

test('late predecessor terminal cannot release a successor bound-session queue', async () => {
  const relocationBehavior = JSON.parse(fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/conformance/relocation-behavior-v1.json'),
    'utf8'
  ));
  const boundSessionBehavior = JSON.parse(fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/conformance/bound-session-relocation-v1.json'),
    'utf8'
  ));
  const behavior = relocationBehavior.idempotencyScenarios.find(
    value => value.name === 'late-terminal-does-not-cross-successor-relocation-fence'
  );
  assert.ok(behavior, 'relocation behavior fixture must define the successor fence scenario');
  assert.deepEqual(
    behavior.successorFenceFields,
    boundSessionBehavior.invariants.successorRelocationFence
  );
  assert.equal(boundSessionBehavior.invariants.latePredecessorTerminalAffectsSuccessor, false);
  const checkpoints = new Map(behavior.checkpoints.map(value => [value.name, value]));

  const actorId = 'actor-successor-session-fence';
  const predecessorSeal = serviceSessionRelocationSeal(actorId);
  const successorRelocationSeal = {
    ...serviceSessionRelocationSeal(actorId),
    relocation: { high: 8n, low: 10n }
  };
  const counts = {
    deliveryCount: 0,
    settlementCount: 0,
    payloadReleaseCount: 0
  };
  const deliveredOperations = [];
  let afterSuccessorSubmit;
  let abortCalls = 0;
  let installedSuccessor = false;
  let drainArrivalAcceptance;
  let successorSeal;
  let successorAcceptance;
  let relay;
  const routeAggregate = await testActorRouteAggregate({
    actorId,
    objectGeneration: 5n,
    meshName: 'play.route',
    nodeRid: 'source',
    bindingGeneration: 6n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    acceptedHighWater: 41n
  }, 2);
  const streamRuntime = {
    ...routeAggregate.port,
    abortActorRouteSeal(requestedActorId, sealId) {
      assert.equal(requestedActorId, actorId);
      abortCalls += 1;
      return routeAggregate.port.abortActorRouteSeal(requestedActorId, sealId);
    },
    find(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return { ref: { acceptedHighWater: 41n } };
    },
    sessionRouteFence(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return {
        actor: {
          actorId,
          objectGeneration: 5n,
          nodeRid: 'source'
        },
        sessionRid: 'session',
        bindingGeneration: 6n,
        authorityOwnerGeneration: 11n,
        ownerLeaseGeneration: 13n,
        acceptedHighWater: 41n
      };
    },
    sendLocalBoundSession(requestedActorId, message) {
      assert.equal(requestedActorId, actorId);
      deliveredOperations.push(message.operation);
      if (message.operation === 'predecessor-first' && !installedSuccessor) {
        installedSuccessor = true;
        drainArrivalAcceptance = relay.receiveRemoteBoundSessionSend(sendPayload(
          'during-predecessor-drain'
        ));
        successorSeal = relay.receiveServiceWireSessionRelocationSeal(
          successorRelocationSeal
        );
        successorAcceptance = successorSeal.then(async () => {
          const accepted = await relay.receiveRemoteBoundSessionSend(sendPayload(
            'successor'
          ));
          afterSuccessorSubmit = { ...counts };
          return accepted;
        });
        return true;
      }
      if (message.operation === 'successor') {
        counts.deliveryCount += 1;
        counts.settlementCount += 1;
        counts.payloadReleaseCount += 1;
      }
      return true;
    }
  };
  routeAggregate.attach(streamRuntime);
  relay = new ZLinkRemoteBoundSessionRelay({
    routeTransport: {},
    streamBindingRuntime: () => streamRuntime,
    actorManager: () => undefined,
    meshRouters: {},
    destroyedActorRefs: new Map(),
    boundSessionFactory() {
      throw new Error('retained sends must use the existing Session route');
    },
    updateRemoteActorPacketTarget() {},
    actorPacketTargetForState: () => undefined
  });
  function sendPayload(operation) {
    return {
      packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
      actorId,
      message: { operation },
      boundPacketName: 'RelocationNotice',
      metadata: {}
    };
  }
  function abortRoute(seal) {
    return {
      relocation: seal.relocation,
      coordinator: seal.coordinator,
      senderRole: 'source',
      actor: seal.actor.actor,
      session: seal.session,
      route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
    };
  }
  function assertCheckpoint(name, actual = counts) {
    const expected = checkpoints.get(name);
    assert.ok(expected, `successor fence fixture must define '${name}'`);
    assert.deepEqual(actual, {
      deliveryCount: expected.deliveryCount,
      settlementCount: expected.settlementCount,
      payloadReleaseCount: expected.payloadReleaseCount
    });
  }

  await relay.receiveServiceWireSessionRelocationSeal(predecessorSeal);
  await relay.receiveRemoteBoundSessionSend(sendPayload('predecessor-first'));
  await relay.receiveRemoteBoundSessionSend(sendPayload('predecessor-second'));
  await Promise.all([
    relay.receiveServiceWireSessionRelocationRoute(abortRoute(predecessorSeal)),
    relay.receiveServiceWireSessionRelocationRoute(abortRoute(predecessorSeal))
  ]);
  assert.equal(abortCalls, 1, 'identical predecessor terminals must share one owner transition');
  await successorSeal;
  assert.deepEqual(await drainArrivalAcceptance, { ok: true });
  assert.deepEqual(await successorAcceptance, { ok: true });
  await waitForCondition(
    () => deliveredOperations.length === 3,
    'predecessor physical FIFO drain before successor terminal'
  );

  assertCheckpoint('afterSuccessorSubmitBeforeOldTerminal', afterSuccessorSubmit);
  await relay.receiveServiceWireSessionRelocationRoute(abortRoute(predecessorSeal));
  assertCheckpoint('afterOldTerminal');
  assert.deepEqual(
    deliveredOperations,
    ['predecessor-first', 'predecessor-second', 'during-predecessor-drain'],
    'the predecessor FIFO may drain physically, but its late terminal cannot release successor traffic'
  );

  await relay.receiveServiceWireSessionRelocationRoute(
    abortRoute(successorRelocationSeal)
  );
  assert.equal(abortCalls, 2);
  await waitForCondition(
    () => deliveredOperations.length === 4,
    'successor FIFO release after successor terminal'
  );
  assertCheckpoint('afterSuccessorTerminal');
  assert.deepEqual(deliveredOperations, [
    'predecessor-first',
    'predecessor-second',
    'during-predecessor-drain',
    'successor'
  ]);

  await Promise.all([
    relay.receiveServiceWireSessionRelocationRoute(abortRoute(predecessorSeal)),
    relay.receiveServiceWireSessionRelocationRoute(abortRoute(successorRelocationSeal))
  ]);
  assert.equal(abortCalls, 2, 'duplicate old and successor terminals must stay terminal');
  assertCheckpoint('afterDuplicateOldAndSuccessorTerminals');

  assert.equal(
    counts.deliveryCount - checkpoints.get('afterOldTerminal').deliveryCount,
    checkpoints.get('afterSuccessorTerminal').deliveryCount
  );
  assert.equal(behavior.oldTerminalAdditionalDeliveryCount, 0);
  assert.equal(behavior.oldTerminalAdditionalSettlementCount, 0);
  assert.equal(behavior.oldTerminalAdditionalPayloadReleaseCount, 0);
});

test('M2 actorJoin A-to-B-to-A successor seal waits until predecessor exact terminal', async () => {
  const actorId = 'actor-serialized-round-trip';
  const predecessorTemplate = serviceSessionRelocationSeal(actorId);
  const predecessor = {
    ...predecessorTemplate,
    coordinator: {
      ...predecessorTemplate.coordinator,
      leaseGeneration: 13n
    }
  };
  const successor = {
    ...predecessor,
    relocation: { high: 17n, low: 19n },
    coordinator: {
      ownerId: 'target-owner',
      leaseGeneration: 14n,
      nodeRid: 'target',
      nodeGeneration: 4n,
      expectedAuthorityStoreVersion: 'store-v18'
    },
    actor: {
      actor: { actorId, generation: 5n, nodeRid: 'target' },
      targetNodeGeneration: 4n,
      authorityOwnerGeneration: 12n,
      ownerLeaseGeneration: 14n
    }
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(new FakeStreamSocket(), 'session', 'public-session')
  );
  await context.actors.bind({
    nodeRid: 'source', actorId, generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const route = serviceSessionRelocationRoute(predecessor, {
    targetNodeRid: 'target',
    targetNodeGeneration: 4n,
    targetAuthorityOwnerGeneration: 12n
  });
  const successorRoute = serviceSessionRelocationRoute(successor, {
    targetNodeRid: 'source',
    targetNodeGeneration: 2n,
    targetAuthorityOwnerGeneration: 13n
  });
  let authorityReads = 0;
  const sentCommands = [];
  let routeCommitCalls = 0;
  let publishFirstCommit;
  const firstCommitPublished = new Promise(resolve => { publishFirstCommit = resolve; });
  let releaseFirstCommit;
  const firstCommitTerminalGate = new Promise(resolve => { releaseFirstCommit = resolve; });
  const commitActorRoute = host.streamBindingRuntime.commitActorRoute
    .bind(host.streamBindingRuntime);
  host.streamBindingRuntime.commitActorRoute = async (...args) => {
    routeCommitCalls += 1;
    const result = await commitActorRoute(...args);
    if (routeCommitCalls === 1) {
      publishFirstCommit();
      await firstCommitTerminalGate;
    }
    return result;
  };
  const relocationRuntime = new ZLinkHostServiceRelocationRuntime({
    locationStore: () => ({
      readAuthority: async () => {
        authorityReads += 1;
        throw new Error('Session owner must not duplicate Location authority validation.');
      }
    }),
    liveDescriptors: async () => [],
    currentOwner: () => ({ ownerId: 'session-owner-id', leaseGeneration: 8n }),
    meshNode: () => ({
      status: () => ({ routingId: 'session-owner', lifecycleGeneration: 4n }),
      peers: () => [
        { routingId: 'source', lifecycleGeneration: 2n, state: 3 },
        { routingId: 'target', lifecycleGeneration: 4n, state: 3 }
      ],
      sendToNode: (_target, bytes) => {
        sentCommands.push(bytes[3]);
        return SubmitResult.Ok;
      }
    }),
    boundSessionRelocation: {
      receiveSeal: value =>
        host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(value),
      receiveRoute: value =>
        host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(value),
      clear: () =>
        host.boundSessionRelay.boundSessions.clearServiceWireSessionRelocations()
    }
  });
  const dispatch = async (sourceNodeRid, bytes) => await relocationRuntime.tryHandleControl(
    'play.route',
    { sourceNodeRid, parts: [toTestMessagePart(bytes)] }
  );

  try {
    assert.equal(await dispatch(
      'source',
      serviceStatefulWire.encodeSessionRelocationSeal(predecessor)
    ), true);
    const routeBytes = serviceStatefulWire.encodeSessionRelocationRoute(route);
    const concurrentRoutePromises = [
      dispatch('target', routeBytes),
      dispatch('target', routeBytes)
    ];
    await firstCommitPublished;
    assert.equal(String((await host.streamBindingRuntime.find(actorId)).ref.nodeRid), 'target');
    let successorAdmitted = false;
    const successorSeal = dispatch(
      'target',
      serviceStatefulWire.encodeSessionRelocationSeal(successor)
    ).then((value) => {
      successorAdmitted = true;
      return value;
    });
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(
      successorAdmitted,
      false,
      'a successor command 42 waits for the predecessor one-way command 44 terminal'
    );
    releaseFirstCommit();
    const concurrentRoutes = await Promise.allSettled(concurrentRoutePromises);
    assert.equal(concurrentRoutes.every(result =>
      result.status === 'fulfilled' && result.value === true
    ), true);
    assert.equal(routeCommitCalls, 1);
    assert.equal(await successorSeal, true);
    assert.equal(successorAdmitted, true);
    assert.equal(await dispatch(
      'source',
      serviceStatefulWire.encodeSessionRelocationRoute(successorRoute)
    ), true);
    assert.equal(String((await host.streamBindingRuntime.find(actorId)).ref.nodeRid), 'source');
    assert.equal(routeCommitCalls, 2);
    assert.equal(await dispatch(
      'source',
      serviceStatefulWire.encodeSessionRelocationRoute(successorRoute)
    ), true);
    assert.equal(routeCommitCalls, 2, 'an exact duplicate command 44 cannot reapply the route');
    assert.equal(authorityReads, 0);
    assert.deepEqual(sentCommands, [
      serviceStatefulWire.M6bServiceWireCommand.sessionRelocationSealed,
      serviceStatefulWire.M6bServiceWireCommand.sessionRelocationSealed
    ]);
  } finally {
    await relocationRuntime.dispose();
  }
});

test('concurrent identical ownership terminals commit and drain one exact seal once', async () => {
  const actorId = 'actor-concurrent-ownership-terminal';
  const seal = serviceSessionRelocationSeal(actorId, {
    relocation: { high: 21n, low: 22n }
  });
  const sealId = serviceSessionSealKey(seal);
  let currentRef = {
    actorId,
    objectGeneration: 5n,
    meshName: 'test.mesh',
    nodeRid: 'source',
    bindingGeneration: 6n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    acceptedHighWater: 41n
  };
  const routeAggregate = await testActorRouteAggregate(currentRef);
  let commitCalls = 0;
  const deliveries = [];
  const streamRuntime = {
    ...routeAggregate.port,
    find(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return { ref: currentRef };
    },
    authorityFence() {
      return {
        authorityOwnerGeneration: currentRef.ownershipGeneration,
        ownerLeaseGeneration: currentRef.ownerLeaseGeneration
      };
    },
    sessionRouteFence(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return {
        actor: currentRef,
        sessionRid: 'session',
        bindingGeneration: 6n
      };
    },
    async commitActorRoute(actorRef, _session, options) {
      commitCalls += 1;
      assert.deepEqual(options.releaseSeal, { sealId });
      await routeAggregate.publish(actorRef, options.releaseSeal);
      currentRef = actorRef;
      await new Promise((resolve) => setImmediate(resolve));
    },
    sendLocalBoundSession(requestedActorId, message) {
      assert.equal(requestedActorId, actorId);
      deliveries.push(message.operation);
      return true;
    }
  };
  routeAggregate.attach(streamRuntime);
  const relay = new ZLinkRemoteBoundSessionRelay({
    routeTransport: {},
    streamBindingRuntime: () => streamRuntime,
    actorManager: () => undefined,
    meshRouters: {},
    destroyedActorRefs: new Map(),
    boundSessionFactory() {
      throw new Error('retained sends must use the existing Session route');
    },
    updateRemoteActorPacketTarget() {},
    actorPacketTargetForState: () => undefined
  });
  await relay.receiveServiceWireSessionRelocationSeal(seal);
  const wrongIdentity = await relay.receiveRemoteBoundSessionSend({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
    actorId,
    relocationSealId: `${sealId}:stale`,
    message: { operation: 'must-not-retain' },
    boundPacketName: 'RelocationNotice',
    metadata: {}
  });
  assert.deepEqual(wrongIdentity, { ok: false });
  assert.deepEqual(await relay.receiveRemoteBoundSessionSend({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
    actorId,
    relocationSealId: sealId,
    message: { operation: 'retained' },
    boundPacketName: 'RelocationNotice',
    metadata: {}
  }), { ok: true });
  const route = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });

  const [first, duplicate] = await Promise.all([
    relay.receiveServiceWireSessionRelocationRoute(route),
    relay.receiveServiceWireSessionRelocationRoute(route)
  ]);
  assert.equal(first, undefined);
  assert.equal(duplicate, undefined);
  assert.equal(commitCalls, 1);
  assert.deepEqual(deliveries, ['retained']);
  assert.deepEqual(await relay.receiveRemoteBoundSessionSend({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
    actorId,
    relocationSealId: sealId,
    message: { operation: 'terminal-replay' },
    boundPacketName: 'RelocationNotice',
    metadata: {}
  }), { ok: true });
  assert.deepEqual(deliveries, ['retained', 'terminal-replay']);
});

test('concurrent identical abort terminals reopen and drain one exact seal once', async () => {
  const actorId = 'actor-concurrent-abort-terminal';
  const seal = serviceSessionRelocationSeal(actorId, {
    relocation: { high: 31n, low: 32n }
  });
  const sealId = serviceSessionSealKey(seal);
  let abortCalls = 0;
  const deliveries = [];
  const routeAggregate = await testActorRouteAggregate({
    actorId,
    objectGeneration: 5n,
    meshName: 'test.mesh',
    nodeRid: 'source',
    bindingGeneration: 6n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    acceptedHighWater: 41n
  });
  const streamRuntime = {
    ...routeAggregate.port,
    sessionRouteFence(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return {
        actor: {
          actorId,
          objectGeneration: 5n,
          nodeRid: 'source'
        },
        sessionRid: 'session',
        bindingGeneration: 6n
      };
    },
    abortActorRouteSeal(requestedActorId, requestedSealId) {
      assert.equal(requestedActorId, actorId);
      abortCalls += 1;
      return routeAggregate.port.abortActorRouteSeal(requestedActorId, requestedSealId);
    },
    sendLocalBoundSession(requestedActorId, message) {
      assert.equal(requestedActorId, actorId);
      deliveries.push(message.operation);
      return true;
    }
  };
  routeAggregate.attach(streamRuntime);
  const relay = new ZLinkRemoteBoundSessionRelay({
    routeTransport: {},
    streamBindingRuntime: () => streamRuntime,
    actorManager: () => undefined,
    meshRouters: {},
    destroyedActorRefs: new Map(),
    boundSessionFactory() {
      throw new Error('retained sends must use the existing Session route');
    },
    updateRemoteActorPacketTarget() {},
    actorPacketTargetForState: () => undefined
  });
  await relay.receiveServiceWireSessionRelocationSeal(seal);
  const retained = operation => relay.receiveRemoteBoundSessionSend({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEND_PACKET,
    actorId,
    message: { operation },
    boundPacketName: 'RelocationNotice',
    metadata: {}
  });
  await retained('retained-first');
  await retained('retained-second');
  const abort = serviceSessionRelocationRoute(seal, { action: 'abort' });

  await relay.receiveServiceWireSessionRelocationRoute({
    ...abort,
    relocation: { ...abort.relocation, low: abort.relocation.low + 1n }
  });
  assert.equal(abortCalls, 0);
  assert.deepEqual(deliveries, []);

  const valid = relay.receiveServiceWireSessionRelocationRoute(abort);
  const conflicting = relay.receiveServiceWireSessionRelocationRoute({
    ...abort,
    route: { ...abort.route, currentAuthorityOwnerGeneration: 12n }
  });
  const duplicate = relay.receiveServiceWireSessionRelocationRoute(abort);
  await assert.rejects(
    conflicting,
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );
  const [first, duplicateResult] = await Promise.all([
    valid,
    duplicate
  ]);
  assert.equal(first, undefined);
  assert.equal(duplicateResult, undefined);
  assert.equal(abortCalls, 1);
  assert.deepEqual(deliveries, ['retained-first', 'retained-second']);
});

test('service-wire relocation single-flights concurrent commands and rejects conflicting bytes before mutation', async () => {
  const socket = new FakeStreamSocket();
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-service-single-flight', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const seal = serviceSessionRelocationSeal('actor-service-single-flight');
  const firstSeal = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  const identicalSeal = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  const conflictingSeal = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal({
      ...seal,
      actor: { ...seal.actor, authorityOwnerGeneration: seal.actor.authorityOwnerGeneration + 1n }
    });
  await assert.rejects(
    conflictingSeal,
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );
  assert.deepEqual(await identicalSeal, await firstSeal);

  const commit = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });
  const applying = host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute({
      relocation: seal.relocation,
      coordinator: seal.coordinator,
      senderRole: 'source',
      actor: seal.actor.actor,
      session: seal.session,
      route: { action: 'abort', currentAuthorityOwnerGeneration: 11n }
    }),
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );
  await applying;
  assert.equal(String((await host.streamBindingRuntime.find('actor-service-single-flight')).ref.nodeRid), 'target');
});

test('failed one-way command 44 native rebind terminalizes the identity and disconnects held payload', async () => {
  const socket = new FakeStreamSocket();
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-service-rebind-failure', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const seal = serviceSessionRelocationSeal('actor-service-rebind-failure');
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  const commit = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });
  context.enterDispatch(serviceRelayDispatchHeader('HeldAcrossFailedRebind'));
  try {
    const relaying = actor.relay(serviceRelayMessage('{"held":true}'));
    const relayingFailure = assert.rejects(relaying, /binding was removed|ingress was held/);
    await new Promise(resolve => setImmediate(resolve));
    socket.bindError = new Error('injected native rebind failure');
    await assert.rejects(
      host.boundSessionRelay.boundSessions
        .receiveServiceWireSessionRelocationRoute(commit),
      /injected native rebind failure/
    );
    assert.equal(await host.streamBindingRuntime.find(actor.actorId), undefined);
    assert.equal(await host.streamBindingRuntime.validateActorRouteSeal(
      actor.actorId,
      serviceSessionSealKey(seal)
    ), false);
    assert.equal(socket.boundActorSends.length, 0);
    await relayingFailure;

    socket.bindError = undefined;
    await host.boundSessionRelay.boundSessions
      .receiveServiceWireSessionRelocationRoute(commit);
    assert.equal(await host.streamBindingRuntime.find(actor.actorId), undefined);
    assert.equal(socket.boundActors.length, 1, 'a terminal duplicate cannot retry the native bind');
    assert.equal(socket.boundActorSends.length, 0);
  } finally {
    context.exitDispatch();
  }
});

test('command 44 without command 42 is a one-way no-op and preserves the active route', async () => {
  const socket = new FakeStreamSocket();
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'session', 'public-session')
  );
  await context.actors.bind({
    nodeRid: 'source', actorId: 'actor-service-no-seal', generation: 5n,
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const seal = serviceSessionRelocationSeal('actor-service-no-seal');
  const route = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'target',
    targetAuthorityOwnerGeneration: 12n
  });
  const result = await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(route);
  assert.equal(result, undefined);
  assert.equal(String((await host.streamBindingRuntime.find('actor-service-no-seal')).ref.nodeRid), 'source');
  assert.equal(socket.boundActors.length, 1);
  assert.equal(await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(route), undefined);
});

test('command 42 exact fences reject stale bindings and command 44 exact retry is idempotent', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-fence', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-fence', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n, acceptedHighWater: 29n
  });
  const seal = serviceSessionRelocationSeal('actor-fence', {
    actorGeneration: 7n,
    sourceNodeRid: 'actor-source',
    authorityOwnerGeneration: 10n,
    ownerLeaseGeneration: 20n,
    sessionRid: 'session-a',
    bindingGeneration: 17n
  });
  for (const stale of [
    {
      ...seal,
      actor: {
        ...seal.actor,
        actor: { ...seal.actor.actor, nodeRid: 'stale-source' }
      }
    },
    {
      ...seal,
      actor: {
        ...seal.actor,
        actor: { ...seal.actor.actor, generation: 8n }
      }
    },
    { ...seal, session: { ...seal.session, bindingGeneration: 18n } },
    { ...seal, session: { ...seal.session, sessionRid: 'session-b' } }
  ]) {
    await assert.rejects(
      host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(stale),
      error => error instanceof framework.ZLinkRemoteBoundSessionFenceError
    );
    assert.equal(String((await host.streamBindingRuntime.find('actor-fence')).ref.nodeRid), 'actor-source');
  }

  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  const route = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'actor-target',
    targetAuthorityOwnerGeneration: 11n
  });
  assert.equal(await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(route), undefined);
  assert.equal(await host.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(route), undefined);
  assert.equal(String((await host.streamBindingRuntime.find('actor-fence')).ref.nodeRid), 'actor-target');
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute({
      ...route,
      route: { ...route.route, targetNodeGeneration: 5n }
    }),
    error => error instanceof ServiceWireProtocolError && /different bytes/.test(error.message)
  );
  assert.equal(String((await host.streamBindingRuntime.find('actor-fence')).ref.nodeRid), 'actor-target');
});

test('command 44 route apply does not require an ActorRef diagnostic high-water copy', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-fence-copy', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-fence-copy', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n
  });
  const seal = serviceSessionRelocationSeal('actor-fence-copy', {
    actorGeneration: 7n,
    sourceNodeRid: 'actor-source',
    authorityOwnerGeneration: 10n,
    ownerLeaseGeneration: 20n,
    sessionRid: 'session-a',
    bindingGeneration: 17n
  });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationSeal(seal);
  const route = serviceSessionRelocationRoute(seal, {
    targetNodeRid: 'actor-target',
    targetAuthorityOwnerGeneration: 11n
  });
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(route);
  await host.boundSessionRelay.boundSessions.receiveServiceWireSessionRelocationRoute(route);

  assert.equal(String((await host.streamBindingRuntime.find('actor-fence-copy')).ref.nodeRid), 'actor-target');
});

test('legacy routed ownership control is not handled or acknowledged', async () => {
  const packetName = '__zlink.actor.bound_session.ownership';
  const requestParts = channelEnvelope.encodeChannelEnvelopeParts(
    1,
    'mesh',
    packetName,
    { packetName, actorId: 'actor-command-44' },
    1000
  ).map(toTestMessagePart);
  let replySubmitted = false;
  const dispatcher = new framework.ZLinkSpotRoutedBoundSessionDispatch({
    channelCodecs: () => undefined
  });
  const handled = await dispatcher.dispatch({
    parts: requestParts,
    requestSeq: 44n,
    reply() {
      return {
        message() { return this; },
        submit() {
          replySubmitted = true;
        }
      };
    }
  });

  assert.equal(handled, false);
  assert.equal(replySubmitted, false);
});

test('runtime host routed bound session receiver forwards through actor remote target when local stream is absent', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const routeCalls = [];
  let resolveRoute;
  const routeReleased = new Promise((resolve) => {
    resolveRoute = resolve;
  });
  host.routeTransport.submitInfrastructure = async (routerChannelId, targetNodeRid, packetName, message, signal) => {
    routeCalls.push({
      routerChannelId,
      targetNodeRid,
      packetName,
      packet: message,
      signal
    });
    await routeReleased;
    return { ok: true };
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-routed-hop');
      return {
        remoteBoundSessionTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'session-node',
          spotId: 'session-entry'
        }
      };
    }
  });

  let completed = false;
  const received = host.createSpotManagerOptions().boundSessionRuntime.receiveRoutedBoundSession(
    'actor-routed-hop',
    { hello: 'routed' },
    'Notify',
    new Map([['seq', '3']])
  ).then(() => {
    completed = true;
  });

  await waitForCondition(() => routeCalls.length === 1, 'routed bound session route submit');
  assert.equal(completed, false);
  resolveRoute();
  await received;
  assert.equal(completed, true);
  assert.equal(routeCalls.length, 1);
  assert.equal(routeCalls[0].routerChannelId, 'room.route');
  assert.equal(routeCalls[0].targetNodeRid, 'session-node');
  assert.equal(routeCalls[0].packetName, '__zlink.actor.bound_session.send');
  assert.equal(routeCalls[0].packet.actorId, 'actor-routed-hop');
  assert.equal(routeCalls[0].packet.boundPacketName, 'Notify');
  assert.deepEqual(routeCalls[0].packet.message, { hello: 'routed' });
  assert.deepEqual(routeCalls[0].packet.metadata, { seq: '3' });
});

test('runtime host actor packet target prefers stored remote room target', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'local-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote-room');
      return {
        spotId: 'room-spot',
        nativeActorRef: {
          nodeRid: 'actor-home-node',
          actorId,
          generation: 1n
        },
        remoteActorPacketTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'room-owner-node',
          spotId: 'room-spot',
          spotKind: framework.ZLinkSpotKind.User
        }
      };
    }
  });

  const target = host.boundSessionRelay.actorPackets.actorPacketTargetForState('actor-remote-room');
  assert.equal(target.routerChannelId, 'room.route');
  assert.equal(target.targetNodeRid, 'room-owner-node');
  assert.equal(String(target.spotId), 'room-spot');
  assert.equal(target.spotKind, framework.ZLinkSpotKind.User);
});

test('remote actor packet target refresh replaces the session actor cache after a Spot transfer', async () => {
  const actor = {
    actorId: 'actor-transfer-target',
    ref: { nodeRid: 'actor-node', actorId: 'actor-transfer-target', generation: 1n }
  };
  const store = new ZLinkRemoteActorPacketTargetStore({
    actorManager: () => undefined,
    streamBindingRuntime: () => ({ find: (actorId) => actorId === actor.actorId ? actor : undefined }),
    meshRouters: {},
    primaryNodeRid: () => 'session-node'
  });
  store.rememberActorTarget(actor, {
    routerChannelId: 'zoneworld.zones',
    targetNodeRid: 'zone-node-1',
    spotId: 'zone-nw',
    spotKind: framework.ZLinkSpotKind.User
  });

  await store.updateFromWire(actor.actorId, {
    routerChannelId: 'zoneworld.zones',
    targetNodeRid: 'zone-node-1',
    spotId: 'zone-sw',
    spotKind: framework.ZLinkSpotKind.User
  });

  assert.equal(String(store.cachedTargetForActor(actor).spotId), 'zone-sw');
});

test('remote actor packet target rejects a reply captured for an older actor tenure', () => {
  const actor = {
    actorId: 'actor-target-tenure',
    ref: {
      nodeRid: 'session-node', actorId: 'actor-target-tenure', meshName: '', objectGeneration: 1n,
      bindingGeneration: 3n, ownershipGeneration: 5n, ownerLeaseGeneration: 7n
    }
  };
  const stateTargets = [];
  const store = new ZLinkRemoteActorPacketTargetStore({
    actorManager: () => ({
      setRemoteActorPacketTarget(target) { stateTargets.push(target); }
    }),
    streamBindingRuntime: () => ({}),
    meshRouters: {},
    primaryNodeRid: () => 'session-node'
  });
  const expected = store.tenureKeyForActor(actor);
  actor.ref = { ...actor.ref, ownershipGeneration: 6n, ownerLeaseGeneration: 8n };

  store.rememberActorTarget(actor, {
    routerChannelId: 'game.route', targetNodeRid: 'node-b', spotId: 'spot-b',
    spotKind: framework.ZLinkSpotKind.User
  }, expected);
  store.clear(actor.actorId, expected, actor);

  assert.equal(store.cachedTargetForActor(actor), undefined);
  assert.deepEqual(stateTargets, []);
});

test('remote actor packet target wire preserves the complete Ready authority fence', async () => {
  const actor = {
    actorId: 'actor-ready-fence',
    ref: { nodeRid: 'actor-node', actorId: 'actor-ready-fence', generation: 1n }
  };
  const target = {
    routerChannelId: 'game.route',
    targetNodeRid: 'game-owner-node',
    spotId: 'game-spot',
    spotKind: framework.ZLinkSpotKind.User,
    targetSpotGeneration: 7n,
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    targetOwnerId: 'owner-17',
    ownerLeaseGeneration: 19n,
    authorityStoreVersion: 'version-23'
  };
  const store = new ZLinkRemoteActorPacketTargetStore({
    actorManager: () => undefined,
    streamBindingRuntime: () => ({ find: (actorId) => actorId === actor.actorId ? actor : undefined }),
    meshRouters: {},
    primaryNodeRid: () => 'session-node'
  });

  await store.updateFromWire(
    actor.actorId,
    actorPacketWire.encodeRemoteActorPacketTarget(target)
  );

  assert.deepEqual(store.cachedTargetForActor(actor), target);
});

test('actor packet target keeps a Ready snapshot across equivalent routing-id instances', () => {
  const target = {
    routerChannelId: 'game.route',
    targetNodeRid: zlink.RoutingId.from('game-owner-node'),
    spotId: zlink.RoutingId.from('game-spot'),
    spotKind: framework.ZLinkSpotKind.User,
    targetSpotGeneration: 7n,
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 13n,
    targetOwnerId: 'owner-17',
    ownerLeaseGeneration: 19n,
    authorityStoreVersion: 'version-23'
  };
  const store = new ZLinkRemoteActorPacketTargetStore({
    actorManager: () => ({
      getState(actorId) {
        assert.equal(actorId, 'actor-ready-fence');
        return {
          spotId: zlink.RoutingId.from('game-spot'),
          remoteActorPacketTarget: target
        };
      }
    }),
    streamBindingRuntime: () => ({ find: () => undefined }),
    meshRouters: {},
    primaryNodeRid: () => 'session-node'
  });

  assert.strictEqual(store.targetForState('actor-ready-fence'), target);
});

//  Spec 12 — a direct payload to an existing Ready Spot uses the Location
//  Store's CURRENT owner route. A cached packet target that still points at
//  the previous Entry membership must not be combined with the new room
//  spot id: that fabricated route has no complete Ready authority fence and
//  the next request fails the fence check instead of re-resolving.
test('runtime host joined Spot route invalidates a stale entry target instead of fabricating a room route', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'session-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote-room');
      return {
        spotId: 'room-spot',
        nativeActorRef: {
          nodeRid: 'relay-node',
          actorId,
          generation: 1n
        },
        remoteActorPacketTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'room-owner-node',
          spotId: 'relay-entry',
          spotKind: framework.ZLinkSpotKind.Entry
        }
      };
    }
  });

  const target = host.boundSessionRelay.actorPackets.actorPacketTargetForState('actor-remote-room');
  assert.equal(target, undefined);
});

test('runtime host actor packet target uses spot mesh when route mesh also exists', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'spot.control' }],
      spotNodes: {
        'spot.service': {
          router: { bind: 'tcp://127.0.0.1:1', routingId: 'session-node' }
        }
      }
    })
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'session-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote-room');
      return {
        spotId: 'room-spot',
        nativeActorRef: {
          nodeRid: 'play-node',
          actorId,
          generation: 1n
        }
      };
    }
  });

  const target = host.boundSessionRelay.actorPackets.actorPacketTargetForState('actor-remote-room');
  assert.equal(target.routerChannelId, 'spot.service');
  assert.equal(String(target.targetNodeRid), 'play-node');
  assert.equal(String(target.spotId), 'room-spot');
  assert.equal(target.spotKind, framework.ZLinkSpotKind.User);
});

test('runtime host remote bound session target does not overwrite actor packet route target', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const state = new framework.ZLinkActorRuntimeState('actor-remote-room');
  const actorPacketTarget = {
    routerChannelId: 'room.route',
    targetNodeRid: 'room-owner-node',
    spotId: 'room-spot',
    spotKind: framework.ZLinkSpotKind.User
  };
  const boundSessionTarget = {
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry'
  };
  state.setRemoteActorPacketTarget(actorPacketTarget);
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote-room');
      return state;
    }
  });

  const options = host.createSpotManagerOptions();
  options.boundSessionRuntime.rememberRemoteBoundSessionTarget('actor-remote-room', boundSessionTarget);

  assert.deepEqual(state.remoteBoundSessionTarget, boundSessionTarget);
  assert.deepEqual(state.remoteActorPacketTarget, actorPacketTarget);
});

test('actor state keeps opaque session binding coordinates across packet target refreshes', () => {
  const state = new framework.ZLinkActorRuntimeState('actor-session-coordinates');
  state.setRemoteBoundSessionTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry',
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000001')
  });

  state.setRemoteBoundSessionTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry'
  });

  assert.equal(String(state.remoteBoundSessionTarget.sessionNodeRid), 'session-node');
  assert.equal(state.remoteBoundSessionTarget.sessionRid.toHex(), '00000001');
});

test('Session binding refresh preserves the staged relocation fence for the same route', () => {
  const target = {
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000001'),
    bindingGeneration: 7n,
    previousAuthorityOwnerGeneration: 11n,
    previousOwnerLeaseGeneration: 13n,
    acceptedHighWater: 17n,
    relocationSealId: 'seal-17',
    acceptedJournalReference: 'journal-17',
    acceptedJournalChecksumCrc32c: 19
  };
  const refreshed = framework.mergeRemoteBoundSessionTarget({
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000001')
  }, target);

  assert.equal(refreshed.relocationSealId, 'seal-17');
  assert.equal(refreshed.acceptedHighWater, 17n);
  assert.equal(refreshed.acceptedJournalReference, 'journal-17');
  assert.equal(refreshed.acceptedJournalChecksumCrc32c, 19);
  assert.equal(refreshed.bindingGeneration, 7n);

  const state = new framework.ZLinkActorRuntimeState('actor-session-transfer-refresh');
  state.setBoundSessionTransferTarget({
    ...target,
    serviceWireRelocation: {
      relocation: { high: 21n, low: 22n },
      coordinator: {
        ownerId: 'owner-a',
        leaseGeneration: 23n,
        nodeRid: 'actor-node',
        nodeGeneration: 24n,
        expectedAuthorityStoreVersion: '25'
      },
      session: {
        sessionOwnerNodeRid: 'session-node',
        sessionOwnerNodeGeneration: 26n,
        sessionOwnerId: 'session-owner',
        sessionOwnerLeaseGeneration: 27n,
        sessionRid: '00000001',
        bindingGeneration: 7n
      }
    }
  });
  state.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('refreshed-session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000001'),
    bindingGeneration: 8n
  });

  assert.equal(state.boundSessionTransferTarget.relocationSealId, 'seal-17');
  assert.equal(
    state.boundSessionTransferTarget.serviceWireRelocation.relocation.high,
    21n
  );
  assert.equal(state.boundSessionTransferTarget.bindingGeneration, 8n);
  assert.equal(String(state.boundSessionTransferTarget.spotId), 'refreshed-session-entry');
});

test('Session binding refresh does not preserve the staged relocation fence when the Session identity changes (successor binding)', () => {
  const state = new framework.ZLinkActorRuntimeState('actor-session-successor-refresh');
  state.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000001'),
    bindingGeneration: 7n,
    relocationSealId: 'seal-predecessor',
    serviceWireRelocation: {
      relocation: { high: 1n, low: 2n },
      coordinator: {
        ownerId: 'owner-a',
        leaseGeneration: 3n,
        nodeRid: 'actor-node',
        nodeGeneration: 4n,
        expectedAuthorityStoreVersion: '5'
      },
      session: {
        sessionOwnerNodeRid: 'session-node',
        sessionOwnerNodeGeneration: 6n,
        sessionOwnerId: 'session-owner',
        sessionOwnerLeaseGeneration: 7n,
        sessionRid: '00000001',
        bindingGeneration: 7n
      }
    }
  });

  // A successor Session binding for the same actor carries a different,
  // explicit sessionRid (spec 48 §125: reconnection creates a new Session
  // that never inherits the previous Session's binding). The previously
  // staged relocation fence must not carry forward onto it.
  state.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000002'),
    bindingGeneration: 9n
  });

  assert.equal(state.boundSessionTransferTarget.relocationSealId, undefined);
  assert.equal(state.boundSessionTransferTarget.serviceWireRelocation, undefined);
  assert.equal(state.boundSessionTransferTarget.bindingGeneration, 9n);
  assert.equal(state.boundSessionTransferTarget.sessionRid.toHex(), '00000002');
});

test('bound-session refresh resolved through the mesh-router producer path carries Session identity and blocks a successor fence', () => {
  // Regression for the gap the injected-identity test above cannot catch:
  // the production bind-refresh path (bindRemoteSession ->
  // resolveRemoteBoundSessionTarget -> MeshRouterResolver) must itself
  // attach sessionNodeRid/sessionRid, not merely accept them when a caller
  // hand-supplies them.
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'room.route' }]
    })
  });
  const sourceNodeRid = zlink.RoutingId.from('session-node');
  const predecessorSessionRid = zlink.RoutingId.fromHex('00000001');
  const successorSessionRid = zlink.RoutingId.fromHex('00000002');

  const predecessorTarget = host.boundSessionRelay.boundSessions.resolveRemoteBoundSessionTarget(
    sourceNodeRid,
    predecessorSessionRid
  );
  assert.notEqual(predecessorTarget, undefined);
  assert.equal(String(predecessorTarget.sessionNodeRid), 'session-node');
  assert.equal(predecessorTarget.sessionRid.toHex(), '00000001');

  const sealedFallback = {
    ...predecessorTarget,
    bindingGeneration: 7n,
    relocationSealId: 'seal-predecessor'
  };

  // Same Session, coordinate-only refresh through the producer path: the
  // resolved sourceSessionRid is unchanged, so the fence must be kept.
  const sameSessionRefresh = host.boundSessionRelay.boundSessions.resolveRemoteBoundSessionTarget(
    sourceNodeRid,
    predecessorSessionRid
  );
  const sameSessionMerged = framework.mergeRemoteBoundSessionTarget(sameSessionRefresh, sealedFallback);
  assert.equal(sameSessionMerged.relocationSealId, 'seal-predecessor');

  // A different sourceSessionRid resolved through the same producer path is
  // a successor Session binding and must not inherit the staged fence.
  const successorRefresh = host.boundSessionRelay.boundSessions.resolveRemoteBoundSessionTarget(
    sourceNodeRid,
    successorSessionRid
  );
  assert.equal(successorRefresh.sessionRid.toHex(), '00000002');
  const successorMerged = framework.mergeRemoteBoundSessionTarget(successorRefresh, sealedFallback);
  assert.equal(successorMerged.relocationSealId, undefined);
});

test('target Actor materialization preserves only an exact bound-session relocation fence', () => {
  const state = new framework.ZLinkActorRuntimeState('actor-session-reentry');
  state.setRemoteActorPacketTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'actor-target',
    spotId: 'room'
  });
  state.setRemoteBoundSessionTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry'
  });
  state.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry',
    sessionNodeRid: 'session-node',
    sessionRid: 'session',
    bindingGeneration: 8n,
    relocationSealId: 'seal-reentry'
  });

  state.prepareForRemoteReentry();

  assert.equal(state.remoteActorPacketTarget, undefined);
  assert.equal(state.remoteBoundSessionTarget, undefined);
  assert.equal(state.boundSessionTransferTarget.relocationSealId, 'seal-reentry');
  assert.equal(state.boundSessionBindingGeneration, 8n);

  const ordinary = new framework.ZLinkActorRuntimeState('actor-ordinary-reentry');
  ordinary.setRemoteActorPacketTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'actor-target',
    spotId: 'room'
  });
  ordinary.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry'
  });
  ordinary.prepareForRemoteReentry();
  assert.equal(ordinary.boundSessionTransferTarget, undefined);
});

test('formal transfer route remains preferred over a lightweight remote bind refresh', () => {
  const transfer = {
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    relocationSealId: 'seal-18',
    acceptedHighWater: 18n,
    acceptedJournalReference: 'journal-18'
  };
  const remote = {
    routerChannelId: 'room.route',
    targetNodeRid: zlink.RoutingId.from('session-node'),
    spotId: zlink.RoutingId.from('session-entry'),
    sessionNodeRid: zlink.RoutingId.from('session-node'),
    sessionRid: zlink.RoutingId.fromHex('00000002')
  };

  assert.equal(
    framework.preferredRemoteBoundSessionTarget(remote, transfer),
    transfer
  );
});

test('actor state applies a later native binding generation to the transfer target', () => {
  const state = new framework.ZLinkActorRuntimeState('actor-session-generation');
  state.setBoundSessionTransferTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry'
  });

  state.setBoundSessionBindingGeneration(17n);

  assert.equal(state.boundSessionBindingGeneration, 17n);
  assert.equal(state.boundSessionTransferTarget.bindingGeneration, 17n);
});

test('actor state does not regress a bound-session generation from a stale Core refresh', () => {
  const state = new framework.ZLinkActorRuntimeState('actor-session-generation-monotonic');
  state.setRemoteBoundSessionTarget({
    routerChannelId: 'room.route',
    targetNodeRid: 'session-node',
    spotId: 'session-entry',
    bindingGeneration: 4n
  });
  state.setBoundSessionBindingGeneration(4n);

  state.setBoundSessionBindingGeneration(1n);

  assert.equal(state.boundSessionBindingGeneration, 4n);
  assert.equal(state.remoteBoundSessionTarget.bindingGeneration, 4n);
});

test('runtime host actor packet target lets local joined actors use native gateway', () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'local-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-local-room');
      return {
        spotId: 'room-spot',
        nativeActorRef: {
          nodeRid: 'local-node',
          actorId,
          generation: 1n
        }
      };
    }
  });

  assert.equal(host.boundSessionRelay.actorPackets.actorPacketTargetForState('actor-local-room'), undefined);
});

test('runtime host local spot join uses the formal MeshNode completion contract for actors with native refs', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const operationId = { high: 0n, low: 1n };
  const actorRid = zlink.RoutingId.from('local-node');
  const roomRid = zlink.RoutingId.from('room-1');
  const submitted = [];
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: actorRid }),
      joinActorSpot(actorRef, targetNodeRid, targetSpotId, targetGeneration, request) {
        submitted.push({
          actorRef,
          targetNodeRid,
          targetSpotId,
          targetGeneration,
          request: Buffer.from(request.payload).toString(),
          contentType: request.contentType
        });
        return operationId;
      }
    },
    primaryMeshCompletions: {
      async submit(operation) {
        const actualOperationId = operation();
        assert.deepEqual(actualOperationId, operationId);
        return {
          terminalResult: 0,
          failureErrno: 0,
          operationKind: framework.OperationKind.ActorJoin,
          kindData: {
            kind: 'actorJoinCompletion',
            joinResult: 0,
            actor: {
              nodeRid: actorRid,
              actorId: 'actor-local-room',
              generation: 4n
            },
            location: {
              actor: {
                nodeRid: actorRid,
                actorId: 'actor-local-room',
                generation: 4n
              },
              spotId: roomRid,
              spotGeneration: 9n,
              membershipEpoch: 3n
            }
          },
          parts: [zlink.Message.from('joined')]
        };
      }
    }
  };
  host.createLocationSpotRouteResolver = () => ({
    async resolve(spotId) {
      assert.equal(spotId, 'room-1');
      return {
        meshName: 'game',
        routerChannelId: 'game.route',
        targetNodeRid: actorRid,
        spotId: roomRid,
        spotKind: framework.ZLinkSpotKind.User,
        targetSpotGeneration: 9n
      };
    }
  });
  const actor = { actorId: 'actor-local-room' };
  const state = new framework.ZLinkActorRuntimeState(actor.actorId);
  const refreshed = [];
  host.streamBindingRuntime.refreshActor = async (actorRef, _signal) => {
    refreshed.push({
      nodeRid: actorRef.nodeRid,
      actorId: actorRef.actorId,
      generation: actorRef.generation
    });
  };
  state.setNativeActorRef({
    nodeRid: actorRid,
    actorId: actor.actorId,
    generation: 4n
  });

  const request = zlink.Message.from('hello');
  const result = await host.createActorManagerOptions()
    .joinCoordinator
    .joinSpot(actor, state, 'room-1', request, undefined, undefined);

  assert.equal(submitted.length, 1);
  assert.equal(submitted[0].actorRef.actorId, 'actor-local-room');
  assert.equal(submitted[0].targetNodeRid.toHex(), actorRid.toHex());
  assert.equal(submitted[0].targetSpotId.toHex(), roomRid.toHex());
  assert.equal(submitted[0].targetGeneration, 9n);
  assert.equal(submitted[0].request, 'hello');
  assert.equal(submitted[0].contentType, 'application/json');
  assert.equal(state.spotId.toHex(), roomRid.toHex());
  assert.equal(result.actor.nodeRid.toHex(), actorRid.toHex());
  assert.equal(result.actor.actorId, 'actor-local-room');
  assert.equal(result.actor.generation, 4n);
  assert.deepEqual(refreshed, []);
  assert.equal(result.reply.getString(), 'joined');
  request.close();
  result.reply.close();
});

test('session actor relay sends header and payload through managed stream SessionRelay route', async () => {
  const socket = new FakeStreamSocket();
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));
  const actor = await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 1n });

  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"x":1}'))
    ));
  } finally {
    context.exitDispatch();
  }

  assert.equal(socket.boundActorSends.length, 1);
  assert.equal(socket.boundActorSends[0].sessionRid, 'backend-rid');
  assert.equal(socket.boundActorSends[0].actorId, 'actor-a');
  assert.equal(socket.boundActorSends[0].parts.length, 2);
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(socket.boundActorSends[0].parts[0].bytes);
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(header.name, 'Move');
  assert.equal(new TextDecoder().decode(socket.boundActorSends[0].parts[1].bytes), '{"x":1}');
});

test('session actor relay waits for an in-progress ownership refresh', async () => {
  const socket = new FakeStreamSocket();
  let bindCount = 0;
  let releaseRefresh;
  const refreshCanFinish = new Promise((resolve) => { releaseRefresh = resolve; });
  let refreshStarted;
  const refreshDidStart = new Promise((resolve) => { refreshStarted = resolve; });
  socket.bindActor = async function bindActor(sessionRid, actor, timeoutMs) {
    bindCount += 1;
    if (bindCount === 2) {
      refreshStarted();
      await refreshCanFinish;
    }
    this.boundActors.push({ sessionRid, actor, timeoutMs });
  };
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session')
  );
  const actor = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-a',
    generation: 1n
  });
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    const refreshing = runtime.refreshActor({
      nodeRid: 'node-b',
      actorId: 'actor-a',
      generation: 1n
    });
    await refreshDidStart;
    const sendsBeforeRelay = socket.boundActorSends.length;
    const relaying = actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"x":2}'))
    ));
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(socket.boundActorSends.length, sendsBeforeRelay);

    releaseRefresh();
    await Promise.all([refreshing, relaying]);
    assert.equal(actor.ref.nodeRid, 'node-b');
    assert.equal(socket.boundActorSends.length, sendsBeforeRelay + 1);
    const relayed = socket.boundActorSends.at(-1);
    assert.equal(relayed.actorId, 'actor-a');
    assert.equal(new TextDecoder().decode(relayed.parts[1].bytes), '{"x":2}');
  } finally {
    context.exitDispatch();
  }
});

test('bound-session response keeps its stream route during an ownership refresh', async () => {
  const socket = new FakeStreamSocket();
  const written = [];
  let unbindCount = 0;
  socket.send = (_sessionRid, message) => {
    written.push(bytesOf(message));
    return true;
  };
  socket.unbindActor = async () => { unbindCount += 1; };
  let bindCount = 0;
  let releaseRefresh;
  const refreshCanFinish = new Promise((resolve) => { releaseRefresh = resolve; });
  let refreshStarted;
  const refreshDidStart = new Promise((resolve) => { refreshStarted = resolve; });
  socket.bindActor = async function bindActor(sessionRid, actor, timeoutMs) {
    bindCount += 1;
    if (bindCount === 2) {
      refreshStarted();
      await refreshCanFinish;
    }
    this.boundActors.push({ sessionRid, actor, timeoutMs });
  };
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext(
    new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session')
  );
  await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-a',
    generation: 1n
  });

  const refreshing = runtime.refreshActor({
    nodeRid: 'node-b',
    actorId: 'actor-a',
    generation: 1n
  });
  await refreshDidStart;

  assert.equal(await runtime.sendLocalBoundSessionResponse(
    'actor-a',
    'Match',
    7n,
    { accepted: true },
    new Map(),
    false
  ), true);
  assert.equal(written.length, 1);
  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.requestSeq, 7n);

  releaseRefresh();
  await refreshing;
  assert.equal((await context.actors.find('actor-a')).ref.nodeRid, 'node-b');
  assert.equal(unbindCount, 0);
});

test('runtime host relays bound remote actor request through route channel and completes local stream response', async () => {
  const actorRef = {
    nodeRid: 'play-node', actorId: 'actor-remote', generation: 7n,
    bindingGeneration: 1n
  };
  const routeRequests = [];
  const stream = recordingStream('session-remote-actor', 'session-node');
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: {
        session: {
          router: { bind: 'tcp://127.0.0.1:1', routingId: 'session-node' }
        }
      }
    })
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'session-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote');
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'play-node',
          spotId: 'play-node',
          spotKind: framework.ZLinkSpotKind.Entry
        }
      };
    }
  });
  host.createActorLocationResolver = () => ({
    resolveDirectActorRoute: async () => ({
      meshName: 'room.route',
      actorRef: {
        actorId: 'actor-remote',
        objectGeneration: 7n,
        meshName: 'room.route',
        nodeRid: 'play-node'
      },
      actorType: 'Player',
      ownerNodeGeneration: 1n,
      ownerId: 'owner-a',
      ownerLeaseGeneration: 1n,
      authorityOwnerGeneration: 1n,
      authorityStoreVersion: '1'
    })
  });
  host.routeTransport.requestRawToSpot = async (remoteAddress, request, options) => {
    const payload = JSON.parse(request.data().toString());
    const routedHeader = streamProtocol.decodeStreamHeader(Buffer.from(payload.header, 'base64'));
    if (routedHeader.name === 'framework.internal.actor-session-bind') {
      return [zlink.Message.from(JSON.stringify({ ok: true, response: { acknowledged: true } }))];
    }
    routeRequests.push({
      routerChannelId: remoteAddress.routerChannelId,
      targetNodeRid: remoteAddress.targetNodeRid,
      spotId: remoteAddress.spotId,
      spotKind: remoteAddress.spotKind,
      packetName: payload.packetName,
      timeoutMs: options.timeoutMs,
      request: payload
    });
    return [zlink.Message.from(JSON.stringify({
      ok: true,
      response: { matched: true }
    }))];
  };

  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind(actorRef);
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 2n,
    name: 'MatchBingoReq',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify({ mode: 'classic' })))
    ));
  } finally {
    context.exitDispatch();
  }

  assert.equal(routeRequests.length, 1);
  assert.equal(routeRequests[0].routerChannelId, 'room.route');
  assert.equal(routeRequests[0].targetNodeRid, 'play-node');
  assert.equal(routeRequests[0].spotId, 'play-node');
  assert.equal(routeRequests[0].spotKind, framework.ZLinkSpotKind.Entry);
  assert.equal(routeRequests[0].packetName, '__zlink.actor.packet.relay');
  assert.equal(routeRequests[0].request.packetName, '__zlink.actor.packet.relay');
  assert.equal(routeRequests[0].request.actorId, 'actor-remote');
  assert.equal(routeRequests[0].request.returnResponse, false);
  assert.equal(routeRequests[0].request.messageFollowContext.request, false);
  assert.equal(routeRequests[0].request.messageFollowContext.objectGeneration, '7');
  assert.equal(typeof routeRequests[0].request.messageFollowContext.correlationId, 'string');
  assert.equal(routeRequests[0].request.messageFollowContext.replyRouteId, undefined);
  assert.match(
    routeRequests[0].request.messageFollowContext.payloadChecksumSha256,
    /^[0-9a-f]{64}$/
  );
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.name, '');
  assert.equal(frame.header.requestSeq, 2n);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { matched: true });
});

test('M1 bound Actor request uses its stored Session route without a per-message Location resolve', async () => {
  const actorId = 'actor-stored-session-route';
  const stream = recordingStream('session-stored-route', 'session-owner');
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'play.route' }]
    })
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('session-owner') })
    }
  };
  host.setActorManager({
    getState(requestedActorId) {
      assert.equal(requestedActorId, actorId);
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'play.route',
          targetNodeRid: 'source',
          spotId: 'source-room',
          spotKind: framework.ZLinkSpotKind.User
        }
      };
    }
  });
  let bindingInstalled = false;
  let locationResolveCount = 0;
  host.createActorLocationResolver = () => ({
    async resolveDirectActorRoute() {
      locationResolveCount += 1;
      if (bindingInstalled) {
        throw new Error('per-message Location resolve is forbidden for a bound Actor request');
      }
      return undefined;
    }
  });
  const routed = [];
  host.routeTransport.requestRawToSpot = async (remoteAddress, request) => {
    const payload = JSON.parse(request.data().toString());
    const header = streamProtocol.decodeStreamHeader(Buffer.from(payload.header, 'base64'));
    if (header.name === 'framework.internal.actor-session-bind') {
      return [zlink.Message.from(JSON.stringify({
        ok: true,
        response: { acknowledged: true }
      }))];
    }
    routed.push({
      targetNodeRid: String(remoteAddress.targetNodeRid),
      spotId: String(remoteAddress.spotId),
      packetName: header.name
    });
    return [zlink.Message.from(JSON.stringify({
      ok: true,
      response: { accepted: true }
    }))];
  };

  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind({
    nodeRid: 'source', actorId, generation: 5n, meshName: 'play.route',
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  });
  const resolvesAtBind = locationResolveCount;
  bindingInstalled = true;
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 1n,
    name: 'StoredRouteRequest',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(Buffer.from('{"request":1}'))
    ));
  } finally {
    context.exitDispatch();
  }

  assert.equal(locationResolveCount, resolvesAtBind);
  assert.deepEqual(routed, [{
    targetNodeRid: 'source',
    spotId: 'source-room',
    packetName: 'StoredRouteRequest'
  }]);
});

test('M1 late bound Actor reply keeps its original Session capability across a rebind', async () => {
  const actorId = 'actor-original-reply-capability';
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const originalStream = recordingStream('session-original', 'session-original-rid');
  const replacementStream = recordingStream('session-replacement', 'session-replacement-rid');
  const originalContext = runtime.createSessionContext(originalStream);
  const replacementContext = runtime.createSessionContext(replacementStream);
  const actorRef = {
    nodeRid: 'source', actorId, generation: 5n, meshName: 'play.route',
    ownershipGeneration: 11n, ownerLeaseGeneration: 13n,
    bindingGeneration: 6n, acceptedHighWater: 41n
  };
  const originalActor = await originalContext.actors.bind(actorRef);
  const replyCapability = await runtime.captureBoundSessionResponseTarget(originalActor);
  assert.ok(replyCapability);
  await replacementContext.actors.bind({ ...actorRef, bindingGeneration: 7n });
  assert.equal(await runtime.captureBoundSessionResponseTarget(originalActor), undefined);

  assert.equal(await replyCapability.sendResponse(
    'OriginalRequest',
    7n,
    { from: 'original-request' },
    new Map()
  ), true);

  assert.equal(originalStream.writes.length, 1);
  assert.equal(replacementStream.writes.length, 0);
  const response = decodeFrame(bytesOf(originalStream.writes[0]));
  assert.equal(response.header.requestSeq, 7n);
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(response.payload)),
    { from: 'original-request' }
  );
});

test('service-wire relocation replaces a learned source packet route before the next bound-session request', async () => {
  const actorId = 'actor-relocated-request';
  const sourceRef = {
    nodeRid: 'source',
    actorId,
    generation: 5n,
    ownershipGeneration: 11n,
    ownerLeaseGeneration: 13n,
    bindingGeneration: 6n,
    acceptedHighWater: 41n,
    meshName: 'play.route'
  };
  const stream = recordingStream('session', 'session-owner');
  const sessionHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'play.route' }]
    })
  });
  sessionHost.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('session-owner') })
    }
  };
  sessionHost.createActorLocationResolver = () => ({
    // A Session owner must still use its command-44 ActorRef while the
    // durable location lookup is converging.
    resolveDirectActorRoute: async () => undefined
  });

  const targetActor = { context: { actorId } };
  const targetRef = {
    nodeRid: 'target',
    actorId,
    generation: 5n,
    objectGeneration: 5n,
    ownershipGeneration: 12n,
    ownerLeaseGeneration: 14n,
    bindingGeneration: 6n,
    acceptedHighWater: 41n,
    meshName: 'play.route'
  };
  const targetPacketTarget = {
    routerChannelId: 'play.route',
    targetNodeRid: 'target',
    spotId: 'game-room',
    spotKind: framework.ZLinkSpotKind.User
  };
  const targetState = {
    actor: targetActor,
    nativeActorRef: targetRef,
    spotId: 'game-room',
    remoteActorPacketTarget: targetPacketTarget,
    remoteBoundSessionTarget: {
      routerChannelId: 'play.route',
      targetNodeRid: 'session-owner',
      spotId: 'session-owner',
      sessionNodeRid: 'session-owner',
      sessionRid: 'session',
      bindingGeneration: 6n
    }
  };
  const targetHost = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'play.route' }]
    })
  });
  targetHost.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('target') })
    }
  };
  targetHost.setActorManager({
    getState(requestedActorId) {
      return requestedActorId === actorId ? targetState : undefined;
    }
  });
  const targetRequestSequences = [];
  targetHost.spotManager = {
    async dispatchRoutedActorPacket(
      spotId,
      requestedActorId,
      parts,
      returnResponse,
      remoteBoundSessionTarget,
      fallbackActorRef
    ) {
      assert.equal(spotId, 'game-room');
      assert.equal(requestedActorId, actorId);
      assert.equal(returnResponse, false);
      const header = streamProtocol.decodeStreamHeader(Buffer.from(parts[0].data()));
      assert.equal(header.name, 'PlaceMarkReq');
      targetRequestSequences.push(header.requestSeq);
      await targetHost.boundSessionRelay.boundSessions.sendActorResponse(
        targetActor,
        header.name,
        header.requestSeq,
        { accepted: true },
        { metadata: new Map(), compressPayload: false },
        remoteBoundSessionTarget,
        fallbackActorRef
      );
    }
  };
  targetHost.routeTransport.submitInfrastructure = async (
    _routerChannelId,
    targetNodeRid,
    packetName,
    payload
  ) => {
    assert.equal(targetNodeRid, 'session-owner');
    assert.equal(packetName, framework.ZLINK_REMOTE_BOUND_SESSION_RESPONSE_PACKET);
    await sessionHost.boundSessionRelay.boundSessions.receiveRemoteBoundSessionResponse(payload);
    return { status: ZLinkSubmitStatus.Submitted };
  };

  let relocated = false;
  let returnedToSource = false;
  const routedTargets = [];
  const routedSpotIds = [];
  sessionHost.routeTransport.sendToSpot = async () => undefined;
  sessionHost.routeTransport.requestRawToSpot = async (remoteAddress, request) => {
    const payload = JSON.parse(request.data().toString());
    const header = streamProtocol.decodeStreamHeader(Buffer.from(payload.header, 'base64'));
    if (header.name === 'framework.internal.actor-session-bind') {
      return [zlink.Message.from(JSON.stringify({
        ok: true,
        response: { acknowledged: true }
      }))];
    }
    routedTargets.push(String(remoteAddress.targetNodeRid));
    routedSpotIds.push(String(remoteAddress.spotId));
    if (!relocated) {
      return [zlink.Message.from(JSON.stringify({
        ok: true,
        response: { accepted: 'source' },
        actorPacketTarget: {
          routerChannelId: 'play.route',
          targetNodeRid: 'source',
          spotId: 'stale-source-spot',
          spotKind: framework.ZLinkSpotKind.User
        }
      }))];
    }
    if (returnedToSource) {
      return [zlink.Message.from(JSON.stringify({
        ok: true,
        response: { accepted: 'returned' }
      }))];
    }
    if (String(remoteAddress.targetNodeRid) === 'source') {
      // Mirrors the stale source shell accepting the one-way relay without
      // owning the relocated handler response.
      return [zlink.Message.from(JSON.stringify({
        ok: true,
        deferredResponse: true
      }))];
    }
    const reply = await targetHost.boundSessionRelay.actorPackets.receiveRemoteActorPacketRelay(
      payload,
      {
        meshName: 'play.route',
        sourceNodeRid: zlink.RoutingId.from('session-owner')
      }
    );
    return [zlink.Message.from(JSON.stringify(reply))];
  };

  const context = sessionHost.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind(sourceRef);
  const relayRequest = async (requestSeq) => {
    context.enterDispatch({
      kind: connector.ZlinkStreamMessageKind.Request,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
      requestSeq,
      name: 'PlaceMarkReq',
      metadata: connector.ZlinkStreamMetadataMap.empty
    });
    try {
      await actor.relay(framework.ZLinkMessage.fromEncoded(
        framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify({ cell: 0 })))
      ));
    } finally {
      context.exitDispatch();
    }
  };

  await relayRequest(1n);
  assert.deepEqual(routedTargets, ['source']);
  assert.deepEqual(routedSpotIds, ['source']);
  stream.writes.splice(0);

  const sealTemplate = serviceSessionRelocationSeal(actorId);
  const seal = {
    ...sealTemplate,
    session: { ...sealTemplate.session, sessionRid: 'session-owner' }
  };
  const sealed = await sessionHost.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationSeal(seal);
  const commit = {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    senderRole: 'target',
    actor: { ...seal.actor.actor, nodeRid: 'target' },
    session: seal.session,
    route: {
      action: 'commit',
      previousAuthorityOwnerGeneration: 11n,
      targetAuthorityOwnerGeneration: 12n,
      targetNodeRid: 'target',
      targetNodeGeneration: 4n
    }
  };
  await sessionHost.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  await sessionHost.boundSessionRelay.boundSessions
    .receiveServiceWireSessionRelocationRoute(commit);
  relocated = true;

  await relayRequest(2n);
  assert.deepEqual(routedTargets, ['source', 'target']);
  assert.deepEqual(routedSpotIds, ['source', 'target']);
  assert.deepEqual(targetRequestSequences, [2n]);
  await waitForCondition(
    () => stream.writes.length === 1,
    'relocated remote bound-session request response'
  );
  const response = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(response.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(response.header.requestSeq, 2n);
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(response.payload)),
    { accepted: true }
  );
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(stream.writes.length, 1, 'the original request correlation must complete once');

  sessionHost.boundSessionRelay.actorPackets.clearRemoteActorPacketTarget(actorId);
  await sessionHost.streamBindingRuntime.commitActorRoute({
    ...sourceRef,
    ownershipGeneration: 13n,
    ownerLeaseGeneration: 15n
  });
  returnedToSource = true;
  stream.writes.splice(0);

  await relayRequest(3n);
  assert.deepEqual(routedTargets, ['source', 'target', 'source']);
  assert.deepEqual(
    routedSpotIds,
    ['source', 'target', 'source'],
    'an A-to-B-to-A ref change must not resurrect the old A packet-target key'
  );
  assert.deepEqual(targetRequestSequences, [2n]);
  assert.equal(stream.writes.length, 1);
  const returnedResponse = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(returnedResponse.header.requestSeq, 3n);
  assert.deepEqual(
    JSON.parse(new TextDecoder().decode(returnedResponse.payload)),
    { accepted: 'returned' }
  );
});

test('runtime host relays bound remote actor send through route channel without waiting for reply', async () => {
  const actorRef = { nodeRid: 'play-node', actorId: 'actor-remote-send', generation: 8n };
  const routeSends = [];
  const stream = recordingStream('session-remote-actor-send', 'session-node');
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      routeChannels: [{ routerChannelId: 'room.route' }],
      spotNodes: {
        session: {
          router: { bind: 'tcp://127.0.0.1:1', routingId: 'session-node' }
        }
      }
    })
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      routingId: 'session-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, 'actor-remote-send');
      return {
        remoteActorPacketTarget: {
          routerChannelId: 'room.route',
          targetNodeRid: 'play-node',
          spotId: 'room-1',
          spotKind: framework.ZLinkSpotKind.User
        }
      };
    }
  });
  host.routeTransport.requestRawToSpot = async (_remoteAddress, request) => {
    const payload = JSON.parse(request.data().toString());
    const routedHeader = streamProtocol.decodeStreamHeader(Buffer.from(payload.header, 'base64'));
    assert.equal(routedHeader.name, 'framework.internal.actor-session-bind');
    return [zlink.Message.from(JSON.stringify({ ok: true, response: { acknowledged: true } }))];
  };
  host.routeTransport.sendToSpot = async (remoteAddress, request, options) => {
    routeSends.push({
      routerChannelId: remoteAddress.routerChannelId,
      targetNodeRid: remoteAddress.targetNodeRid,
      spotId: remoteAddress.spotId,
      spotKind: remoteAddress.spotKind,
      packetName: options.packetName,
      request
    });
  };

  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind(actorRef);
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'LeaveGameMsg',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify({ roomId: 'room-1' })))
    ));
  } finally {
    context.exitDispatch();
  }

  assert.equal(routeSends.length, 1);
  assert.equal(routeSends[0].routerChannelId, 'room.route');
  assert.equal(routeSends[0].targetNodeRid, 'play-node');
  assert.equal(routeSends[0].spotId, 'room-1');
  assert.equal(routeSends[0].spotKind, framework.ZLinkSpotKind.User);
  assert.equal(routeSends[0].packetName, '__zlink.actor.packet.relay');
  assert.equal(routeSends[0].request.packetName, '__zlink.actor.packet.relay');
  assert.equal(routeSends[0].request.actorId, 'actor-remote-send');
  const header = protocolCodecs.ZlinkStreamHeaderCodec.decode(Buffer.from(routeSends[0].request.header, 'base64'));
  assert.equal(header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(header.name, 'LeaveGameMsg');
  assert.deepEqual(JSON.parse(Buffer.from(routeSends[0].request.payload, 'base64').toString()), { roomId: 'room-1' });
  assert.equal(stream.writes.length, 0);
});

test('runtime host completes local bound actor request without native SessionRelay response dependency', async () => {
  const actorRef = { nodeRid: 'play-node', actorId: 'actor-local', generation: 3n };
  const stream = recordingStream('session-local-actor', 'session-node');
  const state = new framework.ZLinkActorRuntimeState(actorRef.actorId);
  const dispatches = [];
  let releaseDispatch;
  const dispatchCanComplete = new Promise((resolve) => { releaseDispatch = resolve; });
  let dispatchStarted;
  const dispatchDidStart = new Promise((resolve) => { dispatchStarted = resolve; });
  let dispatchCompleted;
  const dispatchDidComplete = new Promise((resolve) => { dispatchCompleted = resolve; });
  state.setNativeActorRef(actorRef);
  state.setJoinedSpot('play-node');
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('play-node') }),
      routingId: 'play-node'
    }
  };
  host.setActorManager({
    getState(actorId) {
      assert.equal(actorId, actorRef.actorId);
      return state;
    }
  });
  host.setSpotManager({
    hasActiveSpot(spotId) {
      return spotId === 'play-node';
    },
    async dispatchRoutedActorPacket(spotId, actorId, parts, returnResponse) {
      dispatches.push({
        spotId,
        actorId,
        returnResponse,
        header: protocolCodecs.ZlinkStreamHeaderCodec.decode(bytesOf(parts[0])),
        payload: JSON.parse(new TextDecoder().decode(bytesOf(parts[1])))
      });
      dispatchStarted();
      await dispatchCanComplete;
      dispatchCompleted();
      return { joined: true };
    }
  });

  const context = host.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind(actorRef);
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 5n,
    name: 'JoinGameReq',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    const relaying = actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(Buffer.from(JSON.stringify({ roomId: 'room-a' })))
    ));
    await dispatchDidStart;
    await relaying;
    assert.equal(stream.writes.length, 0);
    releaseDispatch();
    await dispatchDidComplete;
    await new Promise((resolve) => setImmediate(resolve));
  } finally {
    context.exitDispatch();
  }

  assert.equal(dispatches.length, 1);
  assert.equal(dispatches[0].spotId, 'play-node');
  assert.equal(dispatches[0].actorId, 'actor-local');
  assert.equal(dispatches[0].returnResponse, true);
  assert.equal(dispatches[0].header.kind, connector.ZlinkStreamMessageKind.Request);
  assert.equal(dispatches[0].header.name, 'JoinGameReq');
  assert.equal(dispatches[0].header.requestSeq, 5n);
  assert.deepEqual(dispatches[0].payload, { roomId: 'room-a' });
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(bytesOf(stream.writes[0]));
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.name, '');
  assert.equal(frame.header.requestSeq, 5n);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { joined: true });
});

test('bound session send and disconnect use current binding token and stale tokens cannot remove newer binding', async () => {
  const sent = [];
  const disconnected = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    transport: {
      async send(actorId, message, options) {
        sent.push({ actorId, frame: decodeFrame(message.bytes), token: options.bindingToken, packetName: options.packetName });
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async disconnect(actorId, options) {
        disconnected.push({ actorId, token: options.bindingToken });
      }
    }
  });

  const oldContext = runtime.createSessionContext(fakeStream('old-session', 'old-rid'));
  const oldActor = await oldContext.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 1 });
  const oldToken = oldActor.bindingToken;

  const newContext = runtime.createSessionContext(fakeStream('new-session', 'new-rid'));
  await newContext.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 2 });

  await runtime.unbind('actor-a', oldContext, oldToken);

  await runtime.createBoundSession('actor-a').send({ hello: 'world' }).packetName('Hello').submit();
  assert.equal(sent.length, 1);
  assert.equal(sent[0].actorId, 'actor-a');
  assert.equal(sent[0].packetName, 'Hello');
  assert.equal(sent[0].frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(sent[0].frame.header.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(sent[0].frame.header.name, 'Hello');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(sent[0].frame.payload)), { hello: 'world' });
  assert.equal((await runtime.find('actor-a')).ref.generation, 2);

  await runtime.createBoundSession('actor-a').disconnect();
  assert.equal(disconnected.length, 1);
  assert.equal(await runtime.find('actor-a'), undefined);
});

test('session A to B rebind fences stale relay and late disconnect without touching other actors', async () => {
  const notified = [];
  const relayCalls = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    async relay(actor) {
      relayCalls.push(actor.actorId);
      return true;
    },
    async notifyDisconnected(actor) {
      notified.push(actor.actorId);
    }
  });
  const sessionA = runtime.createSessionContext(fakeStream('session-a', 'rid-a'));
  const sessionB = runtime.createSessionContext(fakeStream('session-b', 'rid-b'));
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-x',
    objectGeneration: 7n,
    meshName: 'session-test'
  };
  const stale = await sessionA.actors.bind(actorRef);
  const actorA = await sessionA.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-a',
    objectGeneration: 1n,
    meshName: 'session-test'
  });
  const actorB = await sessionB.actors.bind({
    nodeRid: 'node-b',
    actorId: 'actor-b',
    objectGeneration: 1n,
    meshName: 'session-test'
  });
  const current = await sessionB.actors.bind(actorRef);
  sessionA.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'StaleRelay',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await assert.rejects(
      () => stale.relay(framework.ZLinkMessage.fromEncoded(
        framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"stale":true}'))
      )),
      { kind: framework.ZLinkFrameworkErrorKind.InvalidOperation }
    );
  } finally {
    sessionA.exitDispatch();
  }

  await assert.rejects(
    () => stale.notifyDisconnected(),
    { kind: framework.ZLinkFrameworkErrorKind.InvalidOperation }
  );

  assert.deepEqual(relayCalls, []);
  assert.deepEqual(notified, []);
  assert.equal(await sessionA.actors.find(actorRef.actorId), undefined);
  assert.equal(await sessionA.actors.find(actorA.actorId), actorA);
  assert.equal(await sessionB.actors.find(actorB.actorId), actorB);
  assert.equal(await sessionB.actors.find(actorRef.actorId), current);
  assert.equal(await runtime.find(actorRef.actorId), current);
  assert.equal(current.ref.objectGeneration, 7n);
});

test('stored actor route relays once without actor ref lookup or hidden retry', async () => {
  let actorRefLookups = 0;
  let relayAttempts = 0;
  let acceptRelay = true;
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    actorRefResolver() {
      actorRefLookups += 1;
      throw new Error('Actor ref lookup is forbidden after bind.');
    },
    async relay() {
      relayAttempts += 1;
      return acceptRelay;
    }
  });
  const context = runtime.createSessionContext(fakeStream('session-route', 'route-rid'));
  const actor = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-route',
    objectGeneration: 1n,
    meshName: 'session-test'
  });
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: 'StoredRoute',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await actor.relay(framework.ZLinkMessage.fromEncoded(
      framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"route":"current"}'))
    ));

    acceptRelay = false;
    await assert.rejects(
      () => actor.relay(framework.ZLinkMessage.fromEncoded(
        framework.ZLinkEncodedPayload.from(new TextEncoder().encode('{"route":"stale"}'))
      )),
      { kind: framework.ZLinkFrameworkErrorKind.InvalidOperation }
    );
  } finally {
    context.exitDispatch();
  }

  assert.equal(relayAttempts, 2);
  assert.equal(actorRefLookups, 0);
});

test('logical actor disconnect waits for one callback and keeps the physical connection and other binding', async () => {
  const notified = [];
  let releaseSelected;
  const selectedCanFinish = new Promise((resolve) => { releaseSelected = resolve; });
  let selectedStarted;
  const selectedDidStart = new Promise((resolve) => { selectedStarted = resolve; });
  let closeCalls = 0;
  const stream = {
    ...fakeStream('session-logical', 'logical-rid'),
    async close() { closeCalls += 1; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async notifyDisconnected(actor) {
      notified.push(actor.actorId);
      if (actor.actorId === 'actor-selected') {
        selectedStarted();
        await selectedCanFinish;
      }
    }
  });
  const context = runtime.createSessionContext(stream);
  const replacement = runtime.createSessionContext(fakeStream('session-logical-replacement', 'logical-replacement-rid'));
  const selected = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-selected',
    objectGeneration: 1n,
    meshName: 'session-test'
  });
  const other = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-other',
    objectGeneration: 1n,
    meshName: 'session-test'
  });

  let completed = false;
  const notification = selected.notifyDisconnected().then(() => { completed = true; });
  await selectedDidStart;
  assert.equal(completed, false);
  const rebound = await replacement.actors.bindOrGet(selected.ref);
  assert.equal(rebound.actorId, selected.actorId);
  assert.equal(await context.actors.find(selected.actorId), undefined);
  assert.equal(await replacement.actors.find(selected.actorId), rebound);
  assert.equal(await context.actors.find(other.actorId), other);
  assert.equal(closeCalls, 0);

  releaseSelected();
  await notification;

  assert.equal(completed, true);
  assert.deepEqual(notified, ['actor-selected']);
  assert.equal(await replacement.actors.find(selected.actorId), rebound);
  assert.equal(await context.actors.find(other.actorId), other);
  assert.equal(closeCalls, 0);
});

test('physical disconnect dedupes a racing logical notification and retains actor state inputs', async () => {
  const callbacks = [];
  const memberships = new Set(['actor-selected', 'actor-other']);
  let releaseSelected;
  const selectedCanFinish = new Promise((resolve) => { releaseSelected = resolve; });
  let selectedStarted;
  const selectedDidStart = new Promise((resolve) => { selectedStarted = resolve; });
  const runtime = new framework.ZLinkStreamBindingRuntime({
    actorBindTimeoutMs: 1000,
    async notifyDisconnected(actor) {
      callbacks.push(actor.actorId);
      if (actor.actorId === 'actor-selected') {
        selectedStarted();
        await selectedCanFinish;
      }
      if (actor.actorId === 'actor-other') {
        throw new Error('callback failure');
      }
    }
  });
  const context = runtime.createSessionContext(fakeStream('session-race', 'race-rid'));
  const selected = await context.actors.bind({
    nodeRid: 'node-a',
    actorId: 'actor-selected',
    generation: 11n
  });
  await context.actors.bind({
    nodeRid: 'node-b',
    actorId: 'actor-other',
    generation: 13n
  });

  const logical = selected.notifyDisconnected();
  await selectedDidStart;
  const physical = runtime.cleanup(context);
  releaseSelected();
  await Promise.allSettled([logical, physical]);

  assert.equal(callbacks.filter((actorId) => actorId === 'actor-selected').length, 1);
  assert.equal(callbacks.filter((actorId) => actorId === 'actor-other').length, 1);
  assert.equal(await runtime.find('actor-selected'), undefined);
  assert.equal(await runtime.find('actor-other'), undefined);
  assert.deepEqual([...memberships].sort(), ['actor-other', 'actor-selected']);
  assert.equal(selected.ref.generation, 11n);
});

test('physical disconnect releases the binding lane before its lifecycle callback completes', async () => {
  let releaseNotification;
  const notificationCanFinish = new Promise((resolve) => { releaseNotification = resolve; });
  let notificationStarted;
  const notificationDidStart = new Promise((resolve) => { notificationStarted = resolve; });
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async notifyDisconnected() {
      notificationStarted();
      await notificationCanFinish;
    }
  });
  const previous = runtime.createSessionContext(fakeStream('session-previous', 'previous-rid'));
  const replacement = runtime.createSessionContext(fakeStream('session-replacement', 'replacement-rid'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-reconnect-during-disconnect', generation: 1n };
  await previous.actors.bind(actorRef);

  const cleanup = runtime.cleanup(previous);
  await notificationDidStart;
  const rebound = await replacement.actors.bindOrGet(actorRef);

  assert.equal(rebound.actorId, actorRef.actorId);
  assert.equal(await previous.actors.find(actorRef.actorId), undefined);
  assert.equal(await replacement.actors.find(actorRef.actorId), rebound);

  releaseNotification();
  await cleanup;
  assert.equal(await replacement.actors.find(actorRef.actorId), rebound);
});

test('stream binding runtime can remove actor binding during actor destroy cleanup', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(fakeStream('session-destroy', 'rid-destroy'));
  const actor = await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-destroy', generation: 1 });

  assert.equal(await runtime.find('actor-destroy'), actor);
  assert.equal(await context.actors.find('actor-destroy'), actor);

  await runtime.unbindActor('actor-destroy');

  assert.equal(await runtime.find('actor-destroy'), undefined);
  assert.equal(await context.actors.find('actor-destroy'), undefined);
  await assert.rejects(
    () => runtime.sendBoundSession('actor-destroy', { after: 'destroy' }, 'AfterDestroy', new Map()),
    { kind: framework.ZLinkFrameworkErrorKind.InvalidOperation }
  );
});

test('runtime host completes relayed actor request on captured stream after actor binding cleanup', async () => {
  const stream = recordingStream('session-relay-cleanup', 'session-node');
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      discovery: { registries: ['tcp://127.0.0.1:19000'] },
      routeChannels: ['spot.service']
    })
  });
  runtime.streamBindingRuntime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.streamBindingRuntime.createSessionContext(stream);
  const actor = await context.actors.bind({
    nodeRid: 'play-node',
    actorId: 'actor-cleanup',
    generation: 1
  });

  runtime.routeTransport.requestRawToSpot = async () => {
    runtime.streamBindingRuntime.unbindActor('actor-cleanup');
    return [zlink.Message.from(Buffer.from(JSON.stringify({
      ok: true,
      response: { value: 'pong' }
    })))];
  };

  const payload = zlink.Message.from(Buffer.from(JSON.stringify({ value: 'ping' })));
  await runtime.boundSessionRelay.actorPackets.relayRemoteActorPacket(actor, {
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 7n,
    name: 'ComplexActorReq',
    metadata: { values: new Map([['trace', 'cleanup']]) }
  }, payload);
  payload.close();

  assert.equal(await runtime.streamBindingRuntime.find('actor-cleanup'), undefined);
  assert.equal(stream.writes.length, 1);
  const frame = decodeFrame(stream.writes[0].bytes);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.name, '');
  assert.equal(frame.header.requestSeq, 7n);
  assert.equal(frame.header.metadata.get('trace'), 'cleanup');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { value: 'pong' });
});

test('worker promise continuation resumes through the captured handler turn', async () => {
  const serial = new framework.ZLinkSpotSerialExecutor();
  const worker = new framework.ZLinkWorkerRuntime();
  const events = [];

  await serial.execute(async () => {
    void new framework.DefaultZLinkWorkerCall(
      serial,
      (timeoutMs, signal) => worker.scheduleIo(async () => true, timeoutMs, signal)
    )
      .submit().then(async () => {
        events.push('cleanup');
      });
    await new Promise((resolve) => setImmediate(resolve));
    events.push('response');
  });
  await waitForCondition(() => events.includes('cleanup'), 'detached worker cleanup');

  assert.deepEqual(events, ['cleanup', 'response']);
});

test('runtime host awaits routed actor disconnect notification before session cleanup completes', async () => {
  let releaseSend;
  let sendStarted = false;
  const runtime = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration({
      discovery: { registries: ['tcp://127.0.0.1:19000'] },
      routeChannels: ['spot.service']
    })
  });
  runtime.routeTransport.request = async () => {
    sendStarted = true;
    await new Promise((resolve) => {
      releaseSend = resolve;
    });
  };

  let disconnectCompleted = false;
  const disconnecting = runtime.boundSessionRelay.actorPackets.notifyRemoteActorDisconnected('actor-disconnect-await', {
    routerChannelId: 'spot.service',
    targetNodeRid: 'play-node',
    spotId: 'play-node'
  }).then(() => {
    disconnectCompleted = true;
  });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(sendStarted, true);
  assert.equal(disconnectCompleted, false);
  releaseSend();
  await disconnecting;
  assert.equal(disconnectCompleted, true);
});

test('local bound session error response rejects pending actor request', async () => {
  const stream = recordingStream('session-error-response', 'rid-error-response');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext(stream);
  await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-error', generation: 1 });
  const pending = context.startRequest(1000);
  framework.ZLinkPacket('ErrorContractPacket', {
    payload: {
      type: 'object',
      properties: { requestField: { type: 'string' } },
      required: ['requestField']
    }
  })(class ErrorContractPacket {});

  assert.equal(await runtime.sendLocalBoundSessionError(
    'actor-error',
    'ErrorContractPacket',
    pending.requestSeq,
    new Error('remote actor failed'),
    new Map()
  ), true);

  const frame = decodeFrame(stream.writes[0].bytes);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Error);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {
    code: 'Error',
    message: 'remote actor failed',
  });
  const header = {
    kind: streamProtocol.ZLinkStreamMessageKind.Error,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: pending.requestSeq,
    name: 'ErrorContractPacket',
    metadata: { values: new Map() }
  };
  const payload = {
    data: () => Buffer.from(frame.payload),
    close() {}
  };

  assert.equal(context.tryCompleteResponse(header, payload), true);
  await assert.rejects(
    () => pending.promise,
    /remote actor failed/
  );
});

test('stream session and bound session require packetName for structural payloads', async () => {
  const written = [];
  const sent = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    transport: {
      async send(actorId, message, options) {
        sent.push({ actorId, frame: decodeFrame(message.bytes), packetName: options.packetName });
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async disconnect() {}
    }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-structural-payload', 'rid-structural-payload'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });
  await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-structural', generation: 1 });

  await assert.rejects(
    () => context.client.send({ ok: true }).submit(),
    /Stream packetName is required when the payload type cannot provide one/
  );
  await assert.rejects(
    () => runtime.createBoundSession('actor-structural').send({ ok: true }).submit(),
    /Stream packetName is required when the payload type cannot provide one/
  );
  await context.client.send({ ok: true }).packetName('Ready').submit();
  await runtime.createBoundSession('actor-structural').send({ ok: true }).packetName('ActorReady').submit();

  assert.equal(written.length, 1);
  assert.equal(decodeFrame(written[0]).header.name, 'Ready');
  await waitForCondition(() => sent.length === 1, 'structural bound session send');
  assert.equal(sent.length, 1);
  assert.equal(sent[0].packetName, 'ActorReady');
  assert.equal(sent[0].frame.header.name, 'ActorReady');
});

test('stream session actors bindOrGet preserves a handle only within the same object generation', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime({
    transport: {
      async send() {},
      async disconnect() {}
    }
  });
  const context = runtime.createSessionContext(fakeStream('session-bind-or-get', 'session-rid'));
  const firstRef = { nodeRid: 'node-a', actorId: 'actor-1', generation: 1n };
  const same = await context.actors.bindOrGet(firstRef);
  const again = await context.actors.bindOrGet({ nodeRid: 'node-a', actorId: 'actor-1', generation: 1n });
  const moved = await context.actors.bindOrGet({ nodeRid: 'node-b', actorId: 'actor-1', generation: 2n });

  assert.equal(again, same);
  assert.notEqual(moved, same);
  assert.deepEqual(moved.ref, { nodeRid: 'node-b', actorId: 'actor-1', generation: 2n });
  assert.deepEqual(same.ref, firstRef);
});

test('stream session actor changed-ref bind failure preserves the previous native and logical binding', async () => {
  const operations = [];
  let nativeRef;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(_sessionRid, actor) {
      operations.push(`bind:${actor.nodeRid}:${actor.generation}`);
      if (actor.generation === 2n) throw new Error('replacement bind failed');
      nativeRef = actor;
    },
    async unbindActor(_sessionRid, actorId) {
      operations.push(`unbind:${actorId}`);
      nativeRef = undefined;
    },
    sendBoundActor() { return true; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-rid'));
  const firstRef = { nodeRid: 'node-a', actorId: 'actor-rollback', generation: 1n };
  const first = await context.actors.bindOrGet(firstRef);

  await assert.rejects(
    () => context.actors.bindOrGet({ nodeRid: 'node-b', actorId: firstRef.actorId, generation: 2n }),
    (error) => {
      assert.match(error.message, /bound session ref is stale/);
      assert.match(error.cause.message, /replacement bind failed/);
      return true;
    }
  );

  assert.equal(await context.actors.find(firstRef.actorId), first);
  assert.equal(await runtime.find(firstRef.actorId), first);
  assert.deepEqual(first.ref, firstRef);
  assert.equal(nativeRef.actorId, firstRef.actorId);
  assert.equal(nativeRef.generation, 1n);
  assert.deepEqual(operations, [
    'bind:node-a:1',
    'bind:node-b:2'
  ]);
});

test('stream session cross-context bind failure preserves the previous session transaction', async () => {
  const operations = [];
  let boundSessionRid;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(sessionRid) {
      operations.push(`bind:${sessionRid}`);
      if (sessionRid === 'session-new') throw new Error('new session bind failed');
      boundSessionRid = sessionRid;
    },
    async unbindActor(sessionRid) {
      operations.push(`unbind:${sessionRid}`);
      boundSessionRid = undefined;
    },
    sendBoundActor() { return true; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const previous = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-old'));
  const replacement = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-new'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-cross-session', generation: 1n };
  const actor = await previous.actors.bindOrGet(actorRef);

  await assert.rejects(() => replacement.actors.bindOrGet(actorRef), /new session bind failed/);

  assert.equal(boundSessionRid, 'session-old');
  assert.equal(await previous.actors.find(actorRef.actorId), actor);
  assert.equal(await replacement.actors.find(actorRef.actorId), undefined);
  assert.equal(await runtime.find(actorRef.actorId), actor);
  assert.deepEqual(operations, [
    'bind:session-old',
    'bind:session-new'
  ]);
});

test('stream session actor reconnect atomically replaces the native session binding', async () => {
  const operations = [];
  let boundSessionRid;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(sessionRid) {
      operations.push(`bind:${sessionRid}`);
      boundSessionRid = sessionRid;
    },
    async unbindActor(sessionRid) {
      operations.push(`unbind:${sessionRid}`);
      assert.equal(boundSessionRid, sessionRid);
      boundSessionRid = undefined;
    },
    sendBoundActor() { return true; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const first = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-old'));
  const second = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-new'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-reconnect', generation: 1n };

  await first.actors.bindOrGet(actorRef);
  await second.actors.bindOrGet(actorRef);

  assert.deepEqual(operations, ['bind:session-old', 'bind:session-new']);
  assert.equal(await first.actors.find(actorRef.actorId), undefined);
  assert.equal((await second.actors.find(actorRef.actorId))?.actorId, actorRef.actorId);
});

test('remote binding tombstone removes only the exact native and logical session route', async () => {
  const operations = [];
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(sessionRid) {
      operations.push(`bind:${sessionRid}`);
    },
    async unbindActor(sessionRid) {
      operations.push(`unbind:${sessionRid}`);
    },
    sendBoundActor() { return true; }
  };
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-current'));
  await context.actors.bindOrGet({
    actorId: 'actor-exact-tombstone',
    objectGeneration: 3n,
    meshName: 'mesh-a',
    nodeRid: 'actor-node',
    bindingGeneration: 7n
  });
  const boundRef = (await runtime.find('actor-exact-tombstone')).ref;

  assert.equal(await runtime.retireRemoteBinding(boundRef, 'other-session', 7n), false);
  assert.equal(await runtime.retireRemoteBinding(boundRef, 'session-current', 6n), false);
  assert.equal(await runtime.hasBoundSession('actor-exact-tombstone'), true);
  assert.equal(await runtime.retireRemoteBinding(boundRef, 'session-current', 7n), true);
  assert.equal(await runtime.hasBoundSession('actor-exact-tombstone'), false);
  assert.deepEqual(operations, ['bind:session-current']);
});

test('stream session replacement confirmation failure keeps the new binding current', async () => {
  const operations = [];
  let boundSessionRid;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(sessionRid) {
      operations.push(`bind:${sessionRid}`);
      boundSessionRid = sessionRid;
    },
    async unbindActor(sessionRid) {
      operations.push(`unbind:${sessionRid}`);
      assert.equal(boundSessionRid, sessionRid);
      boundSessionRid = undefined;
    },
    sendBoundActor() { return true; }
  };
  let confirmationCount = 0;
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async confirmRemoteActorSessionBinding() {
      confirmationCount += 1;
      if (confirmationCount === 2) throw new Error('replacement confirmation failed');
    }
  });
  const previous = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-old'));
  const replacement = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-new'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-confirm-rollback', generation: 1n };
  const actor = await previous.actors.bindOrGet(actorRef);

  const rebound = await replacement.actors.bindOrGet(actorRef);
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(boundSessionRid, 'session-new');
  assert.equal(await previous.actors.find(actorRef.actorId), undefined);
  assert.equal(await replacement.actors.find(actorRef.actorId), rebound);
  assert.equal(await runtime.find(actorRef.actorId), rebound);
  assert.deepEqual(operations, [
    'bind:session-old',
    'bind:session-new'
  ]);
});

test('stream session replacement waits for the one-way remote binding submission', async () => {
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor() {},
    async unbindActor() {},
    sendBoundActor() { return true; }
  };
  let confirmationCount = 0;
  let releaseReplacement;
  const replacementSubmitted = new Promise((resolve) => {
    releaseReplacement = resolve;
  });
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async confirmRemoteActorSessionBinding(_actor, _sessionRid, _signal, options) {
      confirmationCount += 1;
      if (confirmationCount === 1) {
        assert.equal(options.waitForAcknowledgement, true);
        return;
      }
      assert.equal(options.waitForAcknowledgement, false);
      await replacementSubmitted;
    }
  });
  const previous = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-old'));
  const replacement = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'session-new'));
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-submit-order', generation: 1n };
  await previous.actors.bindOrGet(actorRef);
  let completed = false;

  const rebound = replacement.actors.bindOrGet(actorRef).then(() => { completed = true; });
  await new Promise((resolve) => setImmediate(resolve));

  assert.equal(completed, false);
  releaseReplacement();
  await rebound;
  assert.equal(completed, true);
  assert.equal(await previous.actors.find(actorRef.actorId), undefined);
  assert.equal((await replacement.actors.find(actorRef.actorId))?.actorId, actorRef.actorId);
});

test('stream session binding confirmation carries the accepted native binding generation', async () => {
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor() {},
    async unbindActor() {},
    sendBoundActor() { return true; }
  };
  const confirmedGenerations = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    async confirmRemoteActorSessionBinding(actor) {
      confirmedGenerations.push(actor.bindingGeneration);
    }
  });
  const previousStream = new framework.ZLinkManagedStream(socket, 'session-old');
  const replacementStream = new framework.ZLinkManagedStream(socket, 'session-new');
  previousStream.actorBindingGeneration = () => 7n;
  replacementStream.actorBindingGeneration = () => 8n;
  const previous = runtime.createSessionContext(previousStream);
  const replacement = runtime.createSessionContext(replacementStream);
  const actorRef = {
    nodeRid: 'node-a',
    actorId: 'actor-confirm-generation',
    generation: 1n,
    bindingGeneration: 6n
  };

  await previous.actors.bindOrGet(actorRef);
  await replacement.actors.bindOrGet(actorRef);

  assert.deepEqual(confirmedGenerations, [7n, 8n]);
});

test('bound session without binding is a retriable framework error', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime({
    transport: {
      async send() {},
      async disconnect() {}
    }
  });

  await assert.rejects(
    () => runtime.createBoundSession('missing').send({}).submit(),
  );
});

test('session client send writes dotnet-compatible JSON stream frame through injected message factory', async () => {
  const written = [];
  const closed = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: {
      createTextMessage(payload) {
        return {
          payload,
          close() {
            closed.push(payload);
          }
        };
      },
      createBinaryMessage(payload) {
        return {
          bytes: payload,
          close() {
            closed.push(payload);
          }
        };
      }
    }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-4', 'rid-4'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await context.client.send({ ok: true }).packetName('Ready').metadata('trace', 'send-1').submit();

  assert.equal(written.length, 1);
  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(frame.header.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(frame.header.name, 'Ready');
  assert.equal(frame.header.metadata.get('trace'), 'send-1');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { ok: true });
  assert.equal(closed.length, 1);
});

test('session client send compress writes dotnet LZ4-pickled stream payload', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-compress-send', 'rid-compress-send'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await context.client.send({ ok: true }).packetName('Ready').compress().submit();

  const frame = decodeFrame(written[0]);
  assert.equal((frame.header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(unpickleLz4(frame.payload))), { ok: true });
});

test('actor reply compression writes a compressed local bound-session response frame', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-actor-reply-compress', 'rid-actor-reply-compress'),
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });
  await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-reply-compress', generation: 1n });

  assert.equal(await runtime.sendLocalBoundSessionResponse(
    'actor-reply-compress',
    'Move',
    42n,
    { accepted: true },
    new Map([['reply-trace-id', 'reply:actor-reply-compress']]),
    true
  ), true);

  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.requestSeq, 42n);
  assert.equal(frame.header.metadata.get('reply-trace-id'), 'reply:actor-reply-compress');
  assert.equal((frame.header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(unpickleLz4(frame.payload))), { accepted: true });
});

test('session client send compress uses configured stream compression codec', async () => {
  const written = [];
  const compression = prefixCompressionCodec('fw');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    streamCompression: { codec: compression }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-custom-compress-send', 'rid-custom-compress-send'),
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await context.client.send({ ok: true }).packetName('Ready').compress().submit();

  const frame = decodeFrame(written[0]);
  assert.equal((frame.header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.equal(new TextDecoder().decode(frame.payload), 'fw:{"ok":true}');
});

test('session client send compress fails when stream compression is disabled', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    streamCompression: { disabled: true }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-disabled-compress-send', 'rid-disabled-compress-send'),
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await assert.rejects(
    () => context.client.send({ ok: true }).packetName('Ready').compress().submit(),
    /compression codec/i
  );
  assert.deepEqual(written, []);
});

test('session client reply writes response frame only while dispatching request packet', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-6', 'rid-6'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await assert.rejects(
    () => context.client.reply({ ok: true }).submit(),
    /Reply is only available/
  );

  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 42n,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty,
    correlationId: 'reply-corr-42'
  });
  try {
    await context.client.reply({ accepted: true }).submit();
    await assert.rejects(
      () => context.client.reply({ accepted: false }).submit(),
      /already has a reply submission/
    );
  } finally {
    context.exitDispatch();
  }

  assert.equal(written.length, 1);
  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.requestSeq, 42n);
  assert.equal(frame.header.name, '');
  assert.equal(frame.header.correlationId, 'reply-corr-42');
  assert.equal(frame.header.metadata.get('trace'), undefined);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), { accepted: true });
});

test('session reply token remains consumed after failed admission', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime({ messageFactory: binaryMessageFactory() });
  const context = runtime.createSessionContext({
    ...fakeStream('session-reply-timeout', 'rid-reply-timeout'),
    async submitRaw() {
      return { status: ZLinkSubmitStatus.TimedOut };
    }
  });
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 45n,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await assert.rejects(
      () => context.client.reply({ accepted: true }).submit(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
    );
    await assert.rejects(
      () => context.client.reply({ accepted: false }).submit(),
      /already has a reply submission/
    );
  } finally {
    context.exitDispatch();
  }
});

test('pre-aborted stream reply claims its token before cancellation without transport admission', async () => {
  let attempts = 0;
  const runtime = new framework.ZLinkStreamBindingRuntime({ messageFactory: binaryMessageFactory() });
  const context = runtime.createSessionContext({
    ...fakeStream('session-reply-abort', 'rid-reply-abort'),
    async submitRaw() {
      attempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });
  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 46n,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  const controller = new AbortController();
  controller.abort();
  try {
    await assert.rejects(
      () => context.client.reply({ accepted: true }).submit(controller.signal),
      (error) => error?.name === 'AbortError'
    );
    await assert.rejects(
      () => context.client.reply({ accepted: false }).submit(),
      /already has a reply submission/
    );
    assert.equal(attempts, 0);
  } finally {
    context.exitDispatch();
  }
});

test('stream send validation and duplicate state win over pre-aborted signals', async () => {
  let attempts = 0;
  const runtime = new framework.ZLinkStreamBindingRuntime({ messageFactory: binaryMessageFactory() });
  const context = runtime.createSessionContext({
    ...fakeStream('session-send-validation', 'rid-send-validation'),
    async submitRaw() {
      attempts += 1;
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });
  const controller = new AbortController();
  controller.abort();

  await assert.rejects(
    () => context.client.send({ invalid: true }).submit(controller.signal),
    /packetName|required/i
  );

  const call = context.client.send({ value: 'first' }).packetName('SessionNotice');
  assert.equal(await call.submit(), undefined);
  await assert.rejects(() => call.submit(controller.signal), (error) => {
    assert.equal(error instanceof framework.ZLinkFrameworkException, true);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    return true;
  });
  assert.equal(attempts, 1);
});

test('session send validates and forwards its per-call admission timeout', async () => {
  const observedTimeouts = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({ messageFactory: binaryMessageFactory() });
  const context = runtime.createSessionContext({
    ...fakeStream('session-send-timeout', 'rid-send-timeout'),
    async submitRaw(_message, _signal, timeoutMs) {
      observedTimeouts.push(timeoutMs);
      return { status: ZLinkSubmitStatus.Submitted };
    }
  });

  for (const invalid of [0, -1, 1.5, Number.POSITIVE_INFINITY, 2_147_483_648]) {
    assert.throws(
      () => context.client.send({ value: invalid }).packetName('Notice').timeout(invalid),
      /integer from 1 through 2147483647/
    );
  }
  await context.client.send({ value: 'short' }).packetName('Notice').timeout(25).submit();
  await context.client.send({ value: 'default' }).packetName('Notice').submit();

  assert.deepEqual(observedTimeouts, [25, undefined]);
});

test('session client reply uses configured stream payload codec', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    streamPayloadCodec: {
      encode(payload) {
        return {
          codec: connector.ZlinkStreamCodec.Protobuf,
          payload: new TextEncoder().encode(`proto:${payload.accepted}`)
        };
      }
    }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-protobuf-reply', 'rid-protobuf-reply'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Protobuf,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 44n,
    name: 'AuthenticateReq',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await context.client.reply({ accepted: true }).submit();
  } finally {
    context.exitDispatch();
  }

  assert.equal(written.length, 1);
  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.codec, connector.ZlinkStreamCodec.Protobuf);
  assert.equal(frame.header.requestSeq, 44n);
  assert.equal(frame.header.name, '');
  assert.equal(new TextDecoder().decode(frame.payload), 'proto:true');
});

test('session client reply compress writes dotnet LZ4-pickled response payload', async () => {
  const written = [];
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory()
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-compress-reply', 'rid-compress-reply'),
    write(message) {
      written.push(message.bytes);
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 43n,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await context.client.reply({ accepted: true }).compress().submit();
  } finally {
    context.exitDispatch();
  }

  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.requestSeq, 43n);
  assert.equal((frame.header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.deepEqual(JSON.parse(new TextDecoder().decode(unpickleLz4(frame.payload))), { accepted: true });
});

test('session client reply compress uses configured stream compression codec', async () => {
  const written = [];
  const compression = prefixCompressionCodec('reply');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    messageFactory: binaryMessageFactory(),
    streamCompression: { codec: compression }
  });
  const context = runtime.createSessionContext({
    ...fakeStream('session-custom-compress-reply', 'rid-custom-compress-reply'),
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  context.enterDispatch({
    kind: connector.ZlinkStreamMessageKind.Request,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: 43n,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  });
  try {
    await context.client.reply({ accepted: true }).compress().submit();
  } finally {
    context.exitDispatch();
  }

  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Response);
  assert.equal(frame.header.requestSeq, 43n);
  assert.equal((frame.header.flags & connector.ZlinkStreamHeaderFlags.PayloadCompressed) !== 0, true);
  assert.equal(new TextDecoder().decode(frame.payload), 'reply:{"accepted":true}');
});

test('session context payloadForHeader uses configured stream compression codec and runtime limit', () => {
  const compression = prefixCompressionCodec('in');
  const runtime = new framework.ZLinkStreamBindingRuntime({
    streamCompression: { codec: compression }
  });
  const context = runtime.createSessionContext(fakeStream('session-custom-inbound', 'rid-custom-inbound'));
  const header = {
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
    name: 'Move',
    metadata: connector.ZlinkStreamMetadataMap.empty
  };

  const payload = context.payloadForHeader(header, bindingMessage('in:{"ok":true}'));
  assert.deepEqual(JSON.parse(new TextDecoder().decode(bytesOf(payload))), { ok: true });

  const oversizedRuntime = new framework.ZLinkStreamBindingRuntime({
    streamCompression: {
      codec: {
        compress(value) {
          return value;
        },
        decompress() {
          return new Uint8Array(64 * 1024 + 1);
        }
      }
    }
  });
  const oversizedContext = oversizedRuntime.createSessionContext(fakeStream('session-custom-too-large', 'rid-custom-too-large'));
  assert.throws(
    () => oversizedContext.payloadForHeader(header, bindingMessage('compressed')),
    /maximum stream payload size/
  );
});

test('session context payloadForHeader fails compressed frames when stream compression is disabled', () => {
  const runtime = new framework.ZLinkStreamBindingRuntime({
    streamCompression: { disabled: true }
  });
  const context = runtime.createSessionContext(fakeStream('session-disabled-inbound', 'rid-disabled-inbound'));

  assert.throws(
    () => context.payloadForHeader({
      kind: connector.ZlinkStreamMessageKind.Send,
      codec: connector.ZlinkStreamCodec.Json,
      flags: connector.ZlinkStreamHeaderFlags.PayloadCompressed,
      name: 'Move',
      metadata: connector.ZlinkStreamMetadataMap.empty
    }, bindingMessage('compressed')),
    /compression codec/i
  );
});

test('session client send uses default binding message factory when one is not supplied', async () => {
  class ReadyPacket {}
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const written = [];
  const context = runtime.createSessionContext({
    ...fakeStream('session-5', 'rid-5'),
    write(message) {
      written.push(message.data());
      return true;
    },
    writeRaw(message) {
      written.push(bytesOf(message));
      return true;
    }
  });

  await context.client.send(new ReadyPacket()).submit();

  const frame = decodeFrame(written[0]);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(frame.header.name, 'ReadyPacket');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(frame.payload)), {});
});

test('stream node runtime does not reuse a disconnected session for the same routing id', async () => {
  const socket = new FakeStreamSocket();
  const contexts = [];
  const dispatched = [];
  const disconnected = [];
  const runtime = createStreamRuntime({
    socket,
    sessionFactory(context) {
      contexts.push(context);
      return {
        context,
        async onDispatch(dispatch) {
          dispatched.push({ context, packetName: dispatch.packetName });
        },
        async onDisconnected() {
          disconnected.push(context);
        }
      };
    }
  });
  runtime.start();

  socket.emitFrame('rid-reused', streamHeader('FirstPacket'), bindingMessage('{}'));
  await waitForCondition(() => dispatched.length === 1, 'first stream dispatch');
  assert.equal(contexts.length, 1);

  runtime.markDisconnected('rid-reused', new Error('old session disconnected'));
  socket.emitFrame('rid-reused', streamHeader('SecondPacket'), bindingMessage('{}'));
  await waitForCondition(() => dispatched.length === 2, 'second stream dispatch');

  assert.equal(contexts.length, 2);
  assert.equal(dispatched[0].context, contexts[0]);
  assert.equal(dispatched[1].context, contexts[1]);
  assert.notEqual(contexts[0], contexts[1]);
  await waitForCondition(() => disconnected.length === 1, 'old stream disconnect');
  await runtime.dispose();
});

test('stream session runtime does not invent correlation ids from request sequences', async () => {
  const socket = new FakeStreamSocket();
  const flowEvents = [];
  const dispatchErrors = [];
  const runtime = createStreamRuntime({
    socket,
    dispatchErrors: {
      flow: {
        accepts: () => true,
        flowCreationEnabled: () => false,
        trace(event) {
          flowEvents.push(event);
        }
      },
      report(error) {
        dispatchErrors.push(error);
      }
    },
    sessionFactory(context) {
      return {
        context,
        async onDispatch(dispatch) {
          if (dispatch.packetName === 'FailRequest') {
            throw new Error('expected dispatch failure');
          }
        }
      };
    }
  });
  runtime.start();

  socket.emitFrame('rid-correlation', streamRequestHeader('OkRequest', 41n), bindingMessage('{}'));
  await waitForCondition(() => flowEvents.length === 3, 'successful request flow events');

  socket.emitFrame('rid-correlation', streamRequestHeader('FailRequest', 42n), bindingMessage('{}'));
  await waitForCondition(
    () => dispatchErrors.length === 1,
    'failed request flow events'
  );

  assert.equal(flowEvents.length, 7);

  assert.deepEqual(
    [...flowEvents, ...dispatchErrors].map((event) => event.correlationId),
    [undefined, undefined, undefined, undefined, undefined, undefined, undefined, undefined]
  );
  await runtime.dispose();
});

function fakeStream(sessionId, routingId) {
  return {
    sessionId,
    routingId,
    localAddr: undefined,
    remoteAddr: undefined,
    write() {
      return true;
    },
    writeRaw() {
      return true;
    },
    async submitRaw(message) {
      return {
        status: this.writeRaw(message)
          ? ZLinkSubmitStatus.Submitted
          : ZLinkSubmitStatus.Backpressured
      };
    },
    async close() {}
  };
}

function createStreamRuntime(options) {
  return new framework.ZLinkStreamSessionNodeRuntime({
    readablePoller: readyPoller(),
    applicationJobQueue: new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration()
    ),
    ...options
  });
}

function readyPoller() {
  return {
    wait() { return true; },
    dispose() {}
  };
}

function recordingStream(sessionId, routingId) {
  const writes = [];
  return {
    sessionId,
    routingId,
    localAddr: undefined,
    remoteAddr: undefined,
    writes,
    write(message) {
      writes.push(message);
      return true;
    },
    writeRaw(message) {
      writes.push(message);
      return true;
    },
    async submitRaw(message) {
      return {
        status: this.writeRaw(message)
          ? ZLinkSubmitStatus.Submitted
          : ZLinkSubmitStatus.Backpressured
      };
    },
    async close() {}
  };
}

function binaryMessageFactory() {
  return {
    createTextMessage(payload) {
      return {
        payload,
        close() {}
      };
    },
    createBinaryMessage(payload) {
      return {
        bytes: payload,
        close() {}
      };
    }
  };
}

function decodeFrame(bytes) {
  const frame = protocolCodecs.ZlinkStreamFrameCodec.decode(bytes);
  return {
    header: protocolCodecs.ZlinkStreamHeaderCodec.decode(frame.header),
    payload: frame.payload
  };
}

function bytesOf(message) {
  if (message.toBytes !== undefined) {
    return message.toBytes();
  }
  if (message.data !== undefined) {
    return message.data();
  }
  if (message.bytes !== undefined) {
    return message.bytes;
  }
  throw new Error('Test message does not expose bytes.');
}

function bindingMessage(payload) {
  const bytes = Buffer.isBuffer(payload) ? payload : Buffer.from(payload);
  return {
    data() {
      return bytes;
    },
    toBytes() {
      return bytes;
    },
    close() {}
  };
}

function streamHeader(packetName) {
  return bindingMessage(streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Send,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    name: packetName,
    metadata: new Map()
  }));
}

function streamRequestHeader(packetName, requestSeq) {
  return bindingMessage(streamProtocol.encodeStreamHeader({
    kind: streamProtocol.ZLinkStreamMessageKind.Request,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.None,
    requestSeq,
    name: packetName,
    metadata: new Map()
  }));
}

function prefixCompressionCodec(prefix) {
  return {
    compress(payload) {
      const body = new TextDecoder().decode(payload);
      return new TextEncoder().encode(`${prefix}:${body}`);
    },
    decompress(payload) {
      const body = new TextDecoder().decode(payload);
      const marker = `${prefix}:`;
      if (!body.startsWith(marker)) {
        throw new Error('Unexpected compression marker.');
      }
      return new TextEncoder().encode(body.slice(marker.length));
    }
  };
}

function unpickleLz4(payload) {
  if (payload.length === 0) {
    return new Uint8Array();
  }
  assert.equal(payload[0], 0);
  return payload.slice(1);
}

async function waitForCondition(predicate, label, timeoutMs = 1000) {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (predicate()) {
      return;
    }
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail(`Timed out waiting for ${label}`);
}

function toTestMessagePart(part) {
  const payload = Buffer.from(typeof part?.data === 'function' ? part.data() : part);
  part?.close?.();
  return {
    data() {
      return payload;
    },
    close() {}
  };
}

async function testActorRouteAggregate(
  initialRef,
  terminalRelocationCapacity,
  outboundCapacity,
  sessionIdentity = 'session'
) {
  const registry = new ZLinkActorSessionBindingRegistry(
    terminalRelocationCapacity,
    outboundCapacity
  );
  const context = {
    routingId: sessionIdentity,
    bindLocal() {},
    unbindLocal() {}
  };
  const bindingToken = 'test-binding';
  let actor = { actorId: initialRef.actorId, ref: initialRef };
  await registry.bind(context, actor, bindingToken, {
    authorityOwnerGeneration: initialRef.ownershipGeneration,
    ownerLeaseGeneration: initialRef.ownerLeaseGeneration,
    ...(initialRef.ownerNodeGeneration === undefined
      ? {}
      : { ownerNodeGeneration: initialRef.ownerNodeGeneration })
  });

  const owner = {
    sealRelocation: (...args) => registry.sealRelocation(...args),
    relocationSnapshot: (actorId, sealId) => registry.relocationSnapshot(actorId, sealId),
    retainRelocationOutbound: (actorId, operation, sealId) =>
      registry.retainRelocationOutbound(actorId, operation, sealId),
    admitRelocationOutbound: (claim, operation) =>
      registry.admitRelocationOutbound(claim, operation),
    discardRelocationOutbound: (actorId, sealId, error) =>
      registry.discardRelocationOutbound(actorId, sealId, error),
    applyRelocation: (...args) => registry.applyRelocation(...args),
    observeRelocationTerminal: (...args) => registry.observeRelocationTerminal(...args),
    clearRelocation: (actorId, error) => registry.clearRelocation(actorId, error),
    async committedRoute(actorId) {
      const route = await registry.route(actorId);
      return route === undefined
        ? undefined
        : { actor: route.actor.ref, authorityFence: route.authorityFence };
    }
  };
  const port = {
    abortActorRouteSeal: (actorId, sealId) => registry.abortSeal(actorId, sealId),
    validateActorRouteSeal: (actorId, sealId) =>
      registry.validateSeal(actorId, sealId)
  };

  return {
    attach(runtime) {
      registerActorSessionBindingRuntimeOwner(runtime, owner);
    },
    owner,
    port,
    async publish(actorRef, releaseSeal) {
      const previous = await registry.route(actorRef.actorId);
      assert.ok(previous);
      const replacement = { actorId: actorRef.actorId, ref: actorRef };
      const authority = {
        authorityOwnerGeneration: actorRef.ownershipGeneration,
        ownerLeaseGeneration: actorRef.ownerLeaseGeneration,
        ...(actorRef.ownerNodeGeneration === undefined
          ? {}
          : { ownerNodeGeneration: actorRef.ownerNodeGeneration })
      };
      if (releaseSeal === undefined) {
        await registry.replace(previous, context, replacement, bindingToken, authority);
      } else {
        await registry.replaceAndReleaseSeal(
          previous,
          context,
          replacement,
          bindingToken,
          releaseSeal.sealId,
          authority
        );
      }
      actor = replacement;
    },
    currentActor: () => actor
  };
}

function testServiceNodeDescriptor(nodeRoutingId, lifecycleGeneration) {
  return {
    meshName: 'play.route',
    nodeRoutingId,
    lifecycleGeneration,
    descriptorRevision: 1n,
    advertisedEndpoint: `inproc://${nodeRoutingId}`,
    channels: [{ name: 'play.route', weight: 100 }],
    state: 'serving',
    securityIdentity: 'test',
    applicationVersion: 1n,
    protocolCapabilities: ['framework-service-v13'],
    objectRole: 'server',
    placementWeight: 100,
    activeCapacityLimit: 100,
    pendingCapacityLimit: 10,
    activeCapacityUsed: 0,
    pendingCapacityUsed: 0
  };
}

function serviceSessionRelocationSeal(actorId, options = {}) {
  const sourceNodeRid = options.sourceNodeRid ?? 'source';
  return {
    relocation: options.relocation ?? { high: 7n, low: 9n },
    coordinator: {
      ownerId: options.coordinatorOwnerId ?? 'coordinator',
      leaseGeneration: options.coordinatorLeaseGeneration ?? 3n,
      nodeRid: options.coordinatorNodeRid ?? sourceNodeRid,
      nodeGeneration: options.coordinatorNodeGeneration ?? 2n,
      expectedAuthorityStoreVersion: options.expectedAuthorityStoreVersion ?? 'store-v17'
    },
    senderRole: 'source',
    actor: {
      actor: {
        actorId,
        generation: options.actorGeneration ?? 5n,
        nodeRid: sourceNodeRid
      },
      targetNodeGeneration: options.sourceNodeGeneration ?? 2n,
      authorityOwnerGeneration: options.authorityOwnerGeneration ?? 11n,
      ownerLeaseGeneration: options.ownerLeaseGeneration ?? 13n
    },
    session: {
      sessionOwnerNodeRid: options.sessionOwnerNodeRid ?? 'session-owner',
      sessionOwnerNodeGeneration: options.sessionOwnerNodeGeneration ?? 4n,
      sessionOwnerId: options.sessionOwnerId ?? 'session-owner-id',
      sessionOwnerLeaseGeneration: options.sessionOwnerLeaseGeneration ?? 8n,
      sessionRid: options.sessionRid ?? 'session',
      bindingGeneration: options.bindingGeneration ?? 6n
    }
  };
}

function serviceSessionRelocationRoute(seal, options = {}) {
  const action = options.action ?? 'commit';
  return {
    relocation: seal.relocation,
    coordinator: seal.coordinator,
    senderRole: action === 'commit' ? 'target' : 'source',
    actor: action === 'commit'
      ? {
          ...seal.actor.actor,
          nodeRid: options.targetNodeRid ?? 'target'
        }
      : seal.actor.actor,
    session: seal.session,
    route: action === 'commit'
      ? {
          action,
          previousAuthorityOwnerGeneration:
            options.previousAuthorityOwnerGeneration ?? seal.actor.authorityOwnerGeneration,
          targetAuthorityOwnerGeneration:
            options.targetAuthorityOwnerGeneration ?? seal.actor.authorityOwnerGeneration + 1n,
          targetNodeRid: options.targetNodeRid ?? 'target',
          targetNodeGeneration: options.targetNodeGeneration ?? 4n
        }
      : {
          action,
          currentAuthorityOwnerGeneration:
            options.currentAuthorityOwnerGeneration ?? seal.actor.authorityOwnerGeneration
        }
  };
}

function serviceSessionSealKey(seal) {
  return serviceStatefulWire.serviceSessionRelocationIdentityKey(seal);
}

function serviceRelayDispatchHeader(packetName) {
  return {
    kind: connector.ZlinkStreamMessageKind.Send,
    codec: connector.ZlinkStreamCodec.Json,
    flags: connector.ZlinkStreamHeaderFlags.None,
    name: packetName,
    metadata: connector.ZlinkStreamMetadataMap.empty
  };
}

function serviceRelayMessage(json) {
  return framework.ZLinkMessage.fromEncoded(
    framework.ZLinkEncodedPayload.from(new TextEncoder().encode(json))
  );
}

class FakeStreamSocket {
  constructor() {
    this.boundActors = [];
    this.unboundActors = [];
    this.boundActorSends = [];
    this.sends = [];
    this.disconnects = [];
    this.bindError = undefined;
    this.received = [];
    this.sendTimeoutMs = -1;
    this.sendHighWaterMark = 4096;
    this.maxMessageSize = 16 * 1024 * 1024;
  }

  send(...args) {
    this.sends.push(args);
    return true;
  }

  async sendAsync(...args) {
    this.sends.push(args);
  }

  disconnectPeer(routingId) {
    this.disconnects.push(routingId);
  }

  onSendReady(handler) {
    this.sendReadyHandler = handler;
  }

  async bindActor(sessionRid, actor, timeoutMs) {
    if (this.bindError !== undefined) {
      throw this.bindError;
    }
    this.boundActors.push({ sessionRid, actor, timeoutMs });
  }

  async unbindActor(_sessionRid, actorId) {
    this.unboundActors.push(actorId);
  }

  sendBoundActor(sessionRid, actorId, parts, flags) {
    this.boundActorSends.push({ sessionRid, actorId, parts, flags });
    return true;
  }

  recv() {
    return this.received.shift();
  }

  emitFrame(routingId, header, payload) {
    const frame = protocolCodecs.ZlinkStreamFrameCodec.encode(
      bytesOf(header),
      bytesOf(payload)
    );
    this.received.push({
      routingId,
      parts: [zlink.Message.from(frame)],
      close() {}
    });
  }

  async dispose() {}
}

class FakeSpotNode {
  constructor(routingId) {
    this.routingId = routingId;
    this.remoteSessionBinds = [];
  }

  bindRemoteActorSession(actor, sourceNodeRid, sourceSessionRid) {
    this.remoteSessionBinds.push({ actor, sourceNodeRid, sourceSessionRid });
  }

  status() {
    return { routingId: zlink.RoutingId.from(this.routingId) };
  }
}
