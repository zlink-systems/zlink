const assert = require('node:assert/strict');
const test = require('node:test');

const connector = require('../../packages/stream-connector/dist');
const protocolCodecs = require('./helpers/stream-protocol-codecs');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const {
  RequestResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const streamProtocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  ZLinkNativeFallbackBoundSession
} = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkRemoteBoundSessionRelay
} = require('../../packages/framework/dist/runtime/host/remote-bound-session-relay');
const {
  ZLinkRemoteActorPacketTargetStore
} = require('../../packages/framework/dist/runtime/host/remote-actor-packet-target-store');
const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');
const actorPacketWire = require('../../packages/framework/dist/runtime/actors/actor-packet-relay-wire');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const zlink = require('@zlink-systems/zlink');

test('stream runtime is exported from framework root surface', () => {
  assert.equal(typeof framework.ZLinkStreamBindingRuntime, 'function');
  assert.equal(typeof framework.DefaultZLinkSessionContext, 'function');
});

test('managed stream binds Session Actors through the Framework service without binding service APIs', async () => {
  const actor = {
    actorId: 'actor-framework-service',
    generation: 7n,
    nodeRid: 'node-play'
  };
  const operations = new Map();
  const bindings = [];
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
    async wait(id) {
      const kind = operations.get(id.low);
      return {
        terminalResult: 0,
        failureErrno: 0,
        operationKind: 0,
        kindData: kind === 'lookup'
          ? { kind: 'actorLookupCompletion', location: { actor } }
          : null,
        parts: []
      };
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
  assert.equal(typeof rawStreamSocket.bindActor, 'undefined');
  assert.equal(typeof rawStreamSocket.unbindActor, 'undefined');
  assert.equal(typeof rawStreamSocket.sendBoundActor, 'undefined');
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
    async wait(id) {
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
  assert.equal(context.actors.find('actor-a'), actor);
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
  assert.equal(context.actors.find('actor-a'), actor);
  assert.equal(runtime.find('actor-a'), actor);
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
  assert.equal(context.actors.find('actor-a'), actor);
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
  socket.bindError = new Error('native bind failed');
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(new framework.ZLinkManagedStream(socket, 'backend-rid', 'public-session'));

  await assert.rejects(
    () => context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-a', generation: 1n }),
    /native bind failed/
  );

  assert.equal(context.actors.find('actor-a'), undefined);
  assert.equal(runtime.find('actor-a'), undefined);
});

test('managed stream remote bind confirmation failure rolls back the accepted native binding', async () => {
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
    () => context.actors.bind({ nodeRid: 'remote-node', actorId: 'actor-relay-fail', generation: 1n }),
    /remote bound session bind confirmation failed/
  );

  assert.equal(nativeActor, undefined);
  assert.equal(context.actors.find('actor-relay-fail'), undefined);
  assert.equal(runtime.find('actor-relay-fail'), undefined);
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

  assert.equal(nativeSends.length, 0);
  assert.equal(stream.writes.length, 1);
  await submit;
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

test('native bound-session send retries only after SEND_READY and preserves submit result', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-ready', generation: 7n };
  let attempts = 0;
  let ready = false;
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        attempts += 1;
        return ready ? zlink.SubmitResult.Ok : zlink.SubmitResult.Backpressured;
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
  assert.equal(attempts, 1, 'backpressured send must not use a polling retry');
  ready = true;
  host.meshSubmitters.notify('__native_bound_session');

  assert.equal(await submit, undefined);
  assert.equal(attempts, 2);
});

test('cancelled native bound-session send is not admitted after SEND_READY', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-cancel', generation: 7n };
  let attempts = 0;
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  host.spotNodeRuntime = {
    primaryMeshNode: {
      status: () => ({ routingId: zlink.RoutingId.from('node-local') }),
      sendActorBoundSession() {
        attempts += 1;
        return zlink.SubmitResult.Backpressured;
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
  await assert.rejects(submit, (error) => error?.name === 'AbortError');
  host.meshSubmitters.notify('__native_bound_session');
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(attempts, 1);
});

test('timed-out native bound-session send reports TimedOut without late admission', async () => {
  const actorRef = { nodeRid: 'node-a', actorId: 'actor-native-timeout', generation: 7n };
  let attempts = 0;
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
        return zlink.SubmitResult.Backpressured;
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
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  assert.equal(attempts, 1);
  host.meshSubmitters.notify('__native_bound_session');
  await new Promise((resolve) => setImmediate(resolve));
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

  host.createActorManagerOptions()
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

  host.createActorManagerOptions()
    .boundSessionFactory(actorRef.actorId)
    .send({ ok: true })
    .packetName('Notify')
    .submit();

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
  host.streamBindingRuntime.commitActorRoute = async (actorRef) => {
    refreshed.push(actorRef);
  };
  host.streamBindingRuntime.rebindActor = async (actorRef) => {
    rebound.push(actorRef);
  };

  await sealSessionRoute(host, 'actor-transfer', 1n, 1n, 7n, 3n, 'seal-transfer');

  await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership({
    actorId: 'actor-transfer',
    actorNodeRid: 'actor-b',
    actorGeneration: '1',
    previousActorOwnershipGeneration: '1',
    actorOwnershipGeneration: '2',
    bindingGeneration: '7',
    previousOwnerLeaseGeneration: '3',
    targetOwnerLeaseGeneration: '4',
    acceptedHighWater: '9',
    sealId: 'seal-transfer',
    acceptedJournalReference: 'journal-transfer',
    acceptedJournalChecksumCrc32c: 1,
    actorPacketTarget: {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-b',
      spotId: 'zone-sw',
      spotKind: framework.ZLinkSpotKind.User
    }
  });
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
    targetRef
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
  assert.equal(refreshed[0].generation, 1n);
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

test('same actor ownership update changes the Spot route without rebinding the active session', async () => {
  const actorRef = {
    nodeRid: 'actor-a', actorId: 'actor-local-move', generation: 1n,
    ownershipGeneration: 1n, ownerLeaseGeneration: 3n,
    bindingGeneration: 7n, acceptedHighWater: 9n
  };
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-local-move', 'session-a'));
  await context.actors.bind(actorRef);
  let refreshed = 0;
  let packetTarget;
  host.streamBindingRuntime.refreshActor = async () => { refreshed += 1; };
  host.boundSessionRelay.actorPackets.updateRemoteActorPacketTarget = (_actorId, target) => {
    packetTarget = target;
  };

  await sealSessionRoute(host, actorRef.actorId, 1n, 1n, 7n, 3n, 'seal-local-move');

  await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership({
    actorId: actorRef.actorId,
    actorNodeRid: actorRef.nodeRid,
    actorGeneration: actorRef.generation.toString(),
    previousActorOwnershipGeneration: '1',
    actorOwnershipGeneration: '2',
    bindingGeneration: '7',
    previousOwnerLeaseGeneration: '3',
    targetOwnerLeaseGeneration: '4',
    acceptedHighWater: '9',
    sealId: 'seal-local-move',
    acceptedJournalReference: 'journal-local-move',
    acceptedJournalChecksumCrc32c: 1,
    actorPacketTarget: {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-a',
      spotId: 'zone-sw',
      spotKind: framework.ZLinkSpotKind.User
    }
  });

  assert.equal(refreshed, 0);
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
  assert.equal(bindingRuntime.find('actor-generation-fence'), original);

  const replacement = await context.actors.bind({
    nodeRid: 'actor-b',
    actorId: 'actor-generation-fence',
    objectGeneration: 2n,
    meshName: 'session-test'
  });
  assert.notEqual(replacement, original);
  assert.equal(replacement.ref.objectGeneration, 2n);
});

test('transferred actor ownership ACK waits until the session route replacement completes', async () => {
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
  host.streamBindingRuntime.commitActorRoute = async () => { await refreshBlocked; };

  await sealSessionRoute(host, 'actor-transfer-ack', 1n, 1n, 7n, 3n, 'seal-ack');

  let settled = false;
  const pending = host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership({
    actorId: 'actor-transfer-ack',
    actorNodeRid: 'actor-b',
    actorGeneration: '1',
    previousActorOwnershipGeneration: '1',
    actorOwnershipGeneration: '2',
    bindingGeneration: '7',
    previousOwnerLeaseGeneration: '3',
    targetOwnerLeaseGeneration: '4',
    acceptedHighWater: '9',
    sealId: 'seal-ack',
    acceptedJournalReference: 'journal-ack',
    acceptedJournalChecksumCrc32c: 1,
    actorPacketTarget: {
      routerChannelId: 'actor.route',
      targetNodeRid: 'actor-b',
      spotId: 'zone-ne',
      spotKind: framework.ZLinkSpotKind.User
    }
  }).then((ack) => {
    settled = true;
    return ack;
  });

  await Promise.resolve();
  assert.equal(settled, false);

  releaseRefresh();
  assert.deepEqual(await pending, {
    actorId: 'actor-transfer-ack',
    actorGeneration: '1',
    actorOwnershipGeneration: '2',
    bindingGeneration: '7',
    targetOwnerLeaseGeneration: '4',
    acceptedHighWater: '9',
    sealId: 'seal-ack'
  });
});

test('command 42 seal is exact and idempotent while command 43 fixes one high-water', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-seal', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-seal', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n, acceptedHighWater: 29n
  });

  const first = await sealSessionRoute(host, 'actor-seal', 7n, 10n, 17n, 20n, 'seal-1');
  const retry = await sealSessionRoute(host, 'actor-seal', 7n, 10n, 17n, 20n, 'seal-1');
  assert.deepEqual(first, { actorId: 'actor-seal', sealId: 'seal-1', acceptedHighWater: '29' });
  assert.deepEqual(retry, first);
  await assert.rejects(
    sealSessionRoute(host, 'actor-seal', 7n, 10n, 18n, 20n, 'seal-2'),
    /sealed by another relocation/
  );
  await assert.rejects(
    host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSeal({
      packetName: framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
      actorId: 'actor-seal',
      actorGeneration: '7',
      actorOwnershipGeneration: '10',
      bindingGeneration: '17',
      ownerLeaseGeneration: '20',
      sealId: 'other-seal'
    }),
    /abort was fenced/
  );
  const released = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSeal({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
    actorId: 'actor-seal',
    actorGeneration: '7',
    actorOwnershipGeneration: '10',
    bindingGeneration: '17',
    ownerLeaseGeneration: '20',
    sealId: 'seal-1'
  });
  const releaseRetry = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSeal({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
    actorId: 'actor-seal',
    actorGeneration: '7',
    actorOwnershipGeneration: '10',
    bindingGeneration: '17',
    ownerLeaseGeneration: '20',
    sealId: 'seal-1'
  });
  assert.deepEqual(releaseRetry, released);
  const next = await sealSessionRoute(host, 'actor-seal', 7n, 10n, 17n, 20n, 'seal-2');
  assert.equal(next.acceptedHighWater, '29');
});

test('command 44 exact fences reject stale bindings and make an exact retry idempotent', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-fence', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-fence', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n, acceptedHighWater: 29n
  });
  const payload = {
    actorId: 'actor-fence',
    actorNodeRid: 'actor-target',
    actorGeneration: '7',
    previousActorOwnershipGeneration: '10',
    actorOwnershipGeneration: '11',
    bindingGeneration: '17',
    previousOwnerLeaseGeneration: '20',
    targetOwnerLeaseGeneration: '21',
    acceptedHighWater: '29',
    sealId: 'seal-fence',
    acceptedJournalReference: 'journal-fence',
    acceptedJournalChecksumCrc32c: 1
  };

  await sealSessionRoute(host, 'actor-fence', 7n, 10n, 17n, 20n, payload.sealId);

  for (const stale of [
    { previousActorOwnershipGeneration: '9' },
    { bindingGeneration: '18' },
    { previousOwnerLeaseGeneration: '19' },
    { acceptedHighWater: '28' }
  ]) {
    await assert.rejects(
      host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership({ ...payload, ...stale }),
      /fenced by its binding identity|did not match its command 42 Session seal/
    );
    assert.equal(String(host.streamBindingRuntime.find('actor-fence').ref.nodeRid), 'actor-source');
  }

  const first = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership(payload);
  const retry = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership(payload);
  assert.deepEqual(retry, first);
  assert.equal(String(host.streamBindingRuntime.find('actor-fence').ref.nodeRid), 'actor-target');
  await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSeal({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_ABORT_SEAL_PACKET,
    actorId: payload.actorId,
    actorGeneration: payload.actorGeneration,
    actorOwnershipGeneration: payload.actorOwnershipGeneration,
    bindingGeneration: payload.bindingGeneration,
    ownerLeaseGeneration: payload.targetOwnerLeaseGeneration,
    sealId: payload.sealId
  });
  const restartedTargetRetry = await host.boundSessionRelay.boundSessions
    .receiveRemoteBoundSessionOwnership(payload);
  assert.deepEqual(restartedTargetRetry, first);
});

test('command 44 uses the binding registry high-water when the ActorRef has no diagnostic copy', async () => {
  const host = new framework.ZLinkFrameworkRuntimeHost({
    registration: framework.createFrameworkRegistration()
  });
  const context = host.streamBindingRuntime.createSessionContext(recordingStream('session-fence-copy', 'session-a'));
  await context.actors.bind({
    nodeRid: 'actor-source', actorId: 'actor-fence-copy', generation: 7n,
    ownershipGeneration: 10n, ownerLeaseGeneration: 20n,
    bindingGeneration: 17n
  });
  const payload = {
    actorId: 'actor-fence-copy',
    actorNodeRid: 'actor-target',
    actorGeneration: '7',
    previousActorOwnershipGeneration: '10',
    actorOwnershipGeneration: '11',
    bindingGeneration: '17',
    previousOwnerLeaseGeneration: '20',
    targetOwnerLeaseGeneration: '21',
    acceptedHighWater: '0',
    sealId: 'seal-fence-copy',
    acceptedJournalReference: 'journal-fence-copy',
    acceptedJournalChecksumCrc32c: 1
  };

  await sealSessionRoute(
    host,
    payload.actorId,
    7n,
    10n,
    17n,
    20n,
    payload.sealId
  );
  const first = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership(payload);
  const retry = await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionOwnership(payload);

  assert.deepEqual(retry, first);
  assert.equal(String(host.streamBindingRuntime.find(payload.actorId).ref.nodeRid), 'actor-target');
});

test('command 44 ownership handler writes command 45 ACK only after route replacement', async () => {
  let releaseReplacement;
  const replacementBlocked = new Promise((resolve) => { releaseReplacement = resolve; });
  const payload = {
    actorId: 'actor-command-44',
    actorNodeRid: 'node-target',
    actorGeneration: '7',
    previousActorOwnershipGeneration: '10',
    actorOwnershipGeneration: '11',
    bindingGeneration: '17',
    previousOwnerLeaseGeneration: '20',
    targetOwnerLeaseGeneration: '21',
    acceptedHighWater: '29',
    sealId: 'seal-handler',
    acceptedJournalReference: 'journal-handler',
    acceptedJournalChecksumCrc32c: 1
  };
  const requestParts = channelEnvelope.encodeChannelEnvelopeParts(
    1,
    'mesh',
    framework.ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET,
    { packetName: framework.ZLINK_REMOTE_BOUND_SESSION_OWNERSHIP_PACKET, ...payload },
    1000
  ).map(toTestMessagePart);
  const replyParts = [];
  let replySubmitted = false;
  const dispatcher = new framework.ZLinkSpotRoutedBoundSessionDispatch({
    channelCodecs: () => undefined,
    routedBoundSessionOwnershipReceiver: async (received) => {
      assert.equal(received.actorId, payload.actorId);
      await replacementBlocked;
      return {
        actorId: received.actorId,
        actorGeneration: received.actorGeneration,
        actorOwnershipGeneration: received.actorOwnershipGeneration,
        bindingGeneration: received.bindingGeneration,
        targetOwnerLeaseGeneration: received.targetOwnerLeaseGeneration,
        acceptedHighWater: received.acceptedHighWater,
        sealId: received.sealId
      };
    }
  });
  const dispatching = dispatcher.dispatch({
    parts: requestParts,
    requestSeq: 44n,
    reply() {
      return {
        message(part) {
          replyParts.push(toTestMessagePart(part));
          return this;
        },
        submit() {
          replySubmitted = true;
        }
      };
    }
  });

  await Promise.resolve();
  assert.equal(replySubmitted, false);
  releaseReplacement();
  assert.equal(await dispatching, true);
  assert.equal(replySubmitted, true);
  const reply = channelEnvelope.decodeChannelEnvelope(replyParts);
  assert.deepEqual(JSON.parse(reply.payload.toString('utf8')), {
    actorId: payload.actorId,
    actorGeneration: payload.actorGeneration,
    actorOwnershipGeneration: payload.actorOwnershipGeneration,
    bindingGeneration: payload.bindingGeneration,
    targetOwnerLeaseGeneration: payload.targetOwnerLeaseGeneration,
    acceptedHighWater: payload.acceptedHighWater,
    sealId: payload.sealId
  });
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

test('remote actor packet target refresh replaces the session actor cache after a Spot transfer', () => {
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

  store.updateFromWire(actor.actorId, {
    routerChannelId: 'zoneworld.zones',
    targetNodeRid: 'zone-node-1',
    spotId: 'zone-sw',
    spotKind: framework.ZLinkSpotKind.User
  });

  assert.equal(String(store.cachedTargetForActor(actor).spotId), 'zone-sw');
});

test('runtime host joined Spot route keeps remote owner node when actor ref is local to a relay node', () => {
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
  assert.equal(target.routerChannelId, 'room.route');
  assert.equal(target.targetNodeRid, 'room-owner-node');
  assert.equal(String(target.spotId), 'room-spot');
  assert.equal(target.spotKind, framework.ZLinkSpotKind.User);
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
          request: Buffer.from(request).toString()
        });
        return operationId;
      }
    },
    primaryMeshCompletions: {
      async wait(actualOperationId) {
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

  assert.equal(runtime.sendLocalBoundSessionResponse(
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
  assert.equal(context.actors.find('actor-a').ref.nodeRid, 'node-b');
  assert.equal(unbindCount, 0);
});

test('runtime host relays bound remote actor request through route channel and completes local stream response', async () => {
  const actorRef = { nodeRid: 'play-node', actorId: 'actor-remote', generation: 7n };
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

  runtime.unbind('actor-a', oldContext, oldToken);

  await runtime.createBoundSession('actor-a').send({ hello: 'world' }).packetName('Hello').submit();
  assert.equal(sent.length, 1);
  assert.equal(sent[0].actorId, 'actor-a');
  assert.equal(sent[0].packetName, 'Hello');
  assert.equal(sent[0].frame.header.kind, connector.ZlinkStreamMessageKind.Send);
  assert.equal(sent[0].frame.header.codec, connector.ZlinkStreamCodec.Json);
  assert.equal(sent[0].frame.header.name, 'Hello');
  assert.deepEqual(JSON.parse(new TextDecoder().decode(sent[0].frame.payload)), { hello: 'world' });
  assert.equal(runtime.find('actor-a').ref.generation, 2);

  await runtime.createBoundSession('actor-a').disconnect();
  assert.equal(disconnected.length, 1);
  assert.equal(runtime.find('actor-a'), undefined);
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
  assert.equal(sessionA.actors.find(actorRef.actorId), undefined);
  assert.equal(sessionA.actors.find(actorA.actorId), actorA);
  assert.equal(sessionB.actors.find(actorB.actorId), actorB);
  assert.equal(sessionB.actors.find(actorRef.actorId), current);
  assert.equal(runtime.find(actorRef.actorId), current);
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
  assert.equal(context.actors.find(other.actorId), other);
  assert.equal(closeCalls, 0);

  releaseSelected();
  await notification;

  assert.equal(completed, true);
  assert.deepEqual(notified, ['actor-selected']);
  assert.equal(context.actors.find(selected.actorId), undefined);
  assert.equal(context.actors.find(other.actorId), other);
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
  assert.equal(runtime.find('actor-selected'), undefined);
  assert.equal(runtime.find('actor-other'), undefined);
  assert.deepEqual([...memberships].sort(), ['actor-other', 'actor-selected']);
  assert.equal(selected.ref.generation, 11n);
});

test('stream binding runtime can remove actor binding during actor destroy cleanup', async () => {
  const runtime = new framework.ZLinkStreamBindingRuntime();
  const context = runtime.createSessionContext(fakeStream('session-destroy', 'rid-destroy'));
  const actor = await context.actors.bind({ nodeRid: 'node-a', actorId: 'actor-destroy', generation: 1 });

  assert.equal(runtime.find('actor-destroy'), actor);
  assert.equal(context.actors.find('actor-destroy'), actor);

  runtime.unbindActor('actor-destroy');

  assert.equal(runtime.find('actor-destroy'), undefined);
  assert.equal(context.actors.find('actor-destroy'), undefined);
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

  assert.equal(runtime.streamBindingRuntime.find('actor-cleanup'), undefined);
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

  assert.equal(runtime.sendLocalBoundSessionError(
    'actor-error',
    'Move',
    pending.requestSeq,
    new Error('remote actor failed'),
    new Map()
  ), true);

  const frame = decodeFrame(stream.writes[0].bytes);
  assert.equal(frame.header.kind, connector.ZlinkStreamMessageKind.Error);
  const header = {
    kind: streamProtocol.ZLinkStreamMessageKind.Error,
    codec: streamProtocol.ZLinkStreamCodec.Json,
    flags: streamProtocol.ZLinkStreamHeaderFlags.HasRequestSeq,
    requestSeq: pending.requestSeq,
    name: 'Move',
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

test('stream session actor changed-ref bind failure restores the previous native and logical binding', async () => {
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

  assert.equal(context.actors.find(firstRef.actorId), first);
  assert.equal(runtime.find(firstRef.actorId), first);
  assert.deepEqual(first.ref, firstRef);
  assert.equal(nativeRef.actorId, firstRef.actorId);
  assert.equal(nativeRef.generation, 1n);
  assert.deepEqual(operations, [
    'bind:node-a:1',
    'unbind:actor-rollback',
    'bind:node-b:2',
    'bind:node-a:1'
  ]);
});

test('stream session cross-context bind failure restores the previous session transaction', async () => {
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
  assert.equal(previous.actors.find(actorRef.actorId), actor);
  assert.equal(replacement.actors.find(actorRef.actorId), undefined);
  assert.equal(runtime.find(actorRef.actorId), actor);
  assert.deepEqual(operations, [
    'bind:session-old',
    'unbind:session-old',
    'bind:session-new',
    'bind:session-old'
  ]);
});

test('stream session actor reconnect unbinds the previous native session before binding the new session', async () => {
  const operations = [];
  let boundSessionRid;
  const socket = {
    send() { return true; },
    disconnectPeer() {},
    recv() { return undefined; },
    async bindActor(sessionRid) {
      operations.push(`bind:${sessionRid}`);
      if (boundSessionRid !== undefined) throw new Error('Binding request failed with result 108.');
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

  const firstActor = await first.actors.bindOrGet(actorRef);
  await firstActor.notifyDisconnected();
  await second.actors.bindOrGet(actorRef);

  assert.deepEqual(operations, ['bind:session-old', 'unbind:session-old', 'bind:session-new']);
  assert.equal(first.actors.find(actorRef.actorId), undefined);
  assert.equal(second.actors.find(actorRef.actorId)?.actorId, actorRef.actorId);
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

  assert.equal(runtime.sendLocalBoundSessionResponse(
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
  await waitForCondition(() => flowEvents.length === 2, 'successful request flow events');

  socket.emitFrame('rid-correlation', streamRequestHeader('FailRequest', 42n), bindingMessage('{}'));
  await waitForCondition(
    () => dispatchErrors.length === 1 && flowEvents.length === 4,
    'failed request flow events'
  );

  assert.deepEqual(
    [...flowEvents, ...dispatchErrors].map((event) => event.correlationId),
    [undefined, undefined, undefined, undefined, undefined]
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

async function sealSessionRoute(
  host,
  actorId,
  actorGeneration,
  actorOwnershipGeneration,
  bindingGeneration,
  ownerLeaseGeneration,
  sealId
) {
  return await host.boundSessionRelay.boundSessions.receiveRemoteBoundSessionSeal({
    packetName: framework.ZLINK_REMOTE_BOUND_SESSION_SEAL_PACKET,
    actorId,
    actorGeneration: actorGeneration.toString(),
    actorOwnershipGeneration: actorOwnershipGeneration.toString(),
    bindingGeneration: bindingGeneration.toString(),
    ownerLeaseGeneration: ownerLeaseGeneration.toString(),
    sealId
  });
}

class FakeStreamSocket {
  constructor() {
    this.boundActors = [];
    this.boundActorSends = [];
    this.bindError = undefined;
    this.received = [];
    this.sendTimeoutMs = -1;
    this.sendHighWaterMark = 4096;
    this.maxMessageSize = 16 * 1024 * 1024;
  }

  send() {
    return true;
  }

  disconnectPeer() {}

  onSendReady(handler) {
    this.sendReadyHandler = handler;
  }

  async bindActor(sessionRid, actor, timeoutMs) {
    if (this.bindError !== undefined) {
      throw this.bindError;
    }
    this.boundActors.push({ sessionRid, actor, timeoutMs });
  }

  async unbindActor() {}

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
