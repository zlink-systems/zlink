const assert = require('node:assert/strict');
const { getEventListeners } = require('node:events');
const test = require('node:test');
const { ZLinkActorRuntimeOptionsFactory } = require('../../packages/framework/dist/runtime/host/actor-runtime-options-factory');
const { DefaultZLinkActorClient } = require('../../packages/framework/dist/runtime/actors/actor-client');
const publication = require('../../packages/framework/dist/runtime/actors/actor-authority-publication');
const relocation = require('../../packages/framework/dist/runtime/foundation/service-relocation-runtime');
const { ZLinkStatefulAuthorityRouteRuntime } = require('../../packages/framework/dist/runtime/host/stateful-authority-route-runtime');
const { ZLinkStoreLocationResolvers } = require('../../packages/framework/dist/runtime/locations/resolvers');
const { ZLinkNodeBackendAdapterFactory } = require('../../packages/framework/dist/runtime/backend');
const { nodeEventLoopPollerOf } = require('../../packages/framework/dist/runtime/backend/node/node-socket-backend-adapter');
const { ZLinkManagedStream } = require('../../packages/framework/dist/runtime/streams/managed-stream');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLINK_INTERNAL_ACTOR_TRANSPORT_DELIVERY_GATE: deliveryGateToken
} = require('../../packages/framework/dist/runtime/actors/actor-transport-delivery-gate');

function authoritySnapshot(version = 'version-a') {
  return {
    kind: 'snapshot',
    payload: publication.encodeActorAuthorityIdentity({
      actorType: 'Player',
      actor: { actorId: 'actor-a', objectGeneration: 17n, meshName: 'game', nodeRid: 'node-a' },
      meshName: 'game',
      ownerNodeGeneration: 23n,
      owner: { ownerId: 'owner-a', leaseGeneration: 29n },
      spotId: 'entry-a', spotGeneration: 23n, spotKind: 'entry'
    }),
    objectGeneration: 17n, authorityOwnerGeneration: 31n,
    ownerId: 'owner-a', ownerLeaseGeneration: 29n, storeVersion: { value: version },
    allocation: {
      state: 'active', objectKind: 'actor', stableType: 'Player',
      descriptor: { meshName: 'game', rid: 'node-a' }, descriptorLifecycleGeneration: 23n
    }
  };
}

test('Actor send and request reuse client options without delivery provider discovery', async () => {
  const snapshot = authoritySnapshot();
  const resolver = new ZLinkStoreLocationResolvers({
    stores: { authorityStore: { async readAuthority() { return snapshot; } } },
    leaseTracker: { async remainingOwnerTokenLeaseMs() { return 100000; } }
  });
  let providerLookups = 0;
  let sends = 0;
  let requests = 0;
  const options = new ZLinkActorRuntimeOptionsFactory({
    registration: { messageSerializers: new Map(), requestTimeoutMs: 1000 },
    providerResolver: { get() { providerLookups += 1; } },
    createActorLocationResolver: () => resolver,
    meshNode: () => ({
      status() { return { routingId: 'node-a' }; },
      async sendToActor() { sends += 1; return SubmitResult.Ok; }
    }),
    meshCompletions: () => undefined,
    actorHandoff: { isKnownStale: () => false }
  }).createActorClientOptions();
  options.handoffCapture = (_mesh, _actor, _parts, returnResponse) => {
    if (!returnResponse) return undefined;
    requests += 1;
    return Promise.resolve({ value: 2 });
  };
  options.staleActorRefPredicate = undefined;
  const client = new DefaultZLinkActorClient(options);
  for (let i = 0; i < 3; i += 1) {
    await client.sendToActor('actor-a', { value: 1 }).packetName('Notify').submit();
    assert.deepEqual(await client.requestToActor('actor-a', { value: 1 }).packetName('Ask').submit(), { value: 2 });
  }
  assert.equal(sends, 3);
  assert.equal(requests, 3);
  assert.equal(providerLookups, 0);
});

for (const present of [false, true]) {
  test(`host selects the delivery gate after providers are ready and reuses it: ${present}`, async (t) => {
    let ready = false;
    let providerLookups = 0;
    const deliveries = [];
    const gate = {
      async waitBeforeSubmit(actorId, kind) { deliveries.push({ actorId, kind }); }
    };
    let registeredGate = present ? gate : undefined;
    const host = new framework.ZLinkFrameworkRuntimeHost({
      registration: framework.createFrameworkRegistration(framework.createFrameworkOptions(() => {})),
      providerResolver: {
        get(token) {
          if (token !== deliveryGateToken) return undefined;
          assert.equal(ready, true, 'delivery provider must not be read during client creation');
          providerLookups += 1;
          return registeredGate;
        }
      }
    });
    t.after(() => host.shutdown());
    const options = host.createActorClientOptions();
    assert.equal(providerLookups, 0);
    ready = true;
    await host.start();
    await host.start();
    assert.equal(providerLookups, 1);
    assert.equal(options.transportDeliveryGate(), registeredGate);
    registeredGate = { async waitBeforeSubmit() { throw new Error('provider was read again'); } };
    const resolver = new ZLinkStoreLocationResolvers({
      stores: { authorityStore: { async readAuthority() { return authoritySnapshot(); } } },
      leaseTracker: { async remainingOwnerTokenLeaseMs() { return 100000; } }
    });
    options.locationResolver = () => resolver;
    options.staleActorRefPredicate = undefined;
    let sends = 0;
    let requests = 0;
    options.nodeProvider = () => ({
      status() { return { routingId: 'node-a' }; },
      async sendToActor() { sends += 1; return SubmitResult.Ok; }
    });
    options.handoffCapture = (_mesh, _actor, _parts, returnResponse) => {
      if (!returnResponse) return undefined;
      requests += 1;
      return Promise.resolve({ value: 2 });
    };
    const client = new DefaultZLinkActorClient(options);
    for (let i = 0; i < 3; i += 1) {
      await client.sendToActor('actor-a', { value: 1 }).packetName('Notify').submit();
      assert.deepEqual(await client.requestToActor('actor-a', { value: 1 }).packetName('Ask').submit(), { value: 2 });
    }
    assert.equal(sends, 3);
    assert.equal(requests, 3);
    assert.equal(providerLookups, 1, 'send and request perform no provider discovery');
    assert.deepEqual(deliveries, present ? Array.from({ length: 3 }, () => [
      { actorId: 'actor-a', kind: 'oneWay' }, { actorId: 'actor-a', kind: 'request' }
    ]).flat() : []);
  });
}

test('canonical Actor authority unwraps once on route miss and creates no legacy mismatch exception', async (t) => {
  const snapshot = authoritySnapshot();
  const originalUnwrap = relocation.serviceRelocationAuthorityApplicationPayload;
  const OriginalTypeError = global.TypeError;
  let unwraps = 0;
  let exceptions = 0;
  t.after(() => {
    relocation.serviceRelocationAuthorityApplicationPayload = originalUnwrap;
    global.TypeError = OriginalTypeError;
  });
  relocation.serviceRelocationAuthorityApplicationPayload = (...args) => {
    unwraps += 1;
    return originalUnwrap(...args);
  };
  global.TypeError = class extends OriginalTypeError {
    constructor(...args) { super(...args); exceptions += 1; }
  };
  const resolver = new ZLinkStoreLocationResolvers({
    stores: { authorityStore: { async readAuthority() { return snapshot; } } },
    leaseTracker: { async remainingOwnerTokenLeaseMs() { return 100000; } }
  });
  assert.equal((await resolver.resolveDirectActorRoute('actor-a')).kind, 'ready');
  assert.equal(unwraps, 1);
  assert.equal(exceptions, 0);
  assert.equal((await resolver.resolveDirectActorRoute('actor-a')).kind, 'ready');
  assert.equal(unwraps, 1);
  const codec = new relocation.ServiceRelocationAuthorityPayloadCodec();
  assert.equal(codec.read(snapshot.payload), undefined);
  assert.equal(exceptions, 0);
  assert.equal(publication.decodeActorAuthorityIdentity(Buffer.from('invalid'), 17n), undefined);
});

test('relocation authority preserves canonical and legacy envelope acceptance and rejection', () => {
  const codec = new relocation.ServiceRelocationAuthorityPayloadCodec();
  const value = {
    reference: 'relocation-root', checksumCrc32c: 7,
    aggregateId: '11111111-1111-4111-8111-111111111111', aggregateGeneration: 2n,
    inventoryDigest: 'a'.repeat(64), targetOwnerId: 'owner-a', targetOwnerLeaseGeneration: 13n
  };
  const canonical = authoritySnapshot().payload;
  const legacyApplication = Buffer.from('legacy-application');
  for (const application of [canonical, legacyApplication]) {
    const wrapped = codec.publish(application, value);
    assert.equal(codec.read(wrapped).reference, value.reference);
    assert.deepEqual(relocation.serviceRelocationAuthorityApplicationPayload(wrapped), application);
    assert.deepEqual(codec.clear(wrapped, value.reference), application);
    assert.equal(codec.read(wrapped.subarray(0, wrapped.length - 1)), undefined);
    assert.throws(() => codec.clear(wrapped, 'other-root'), /reference changed/);
  }
});

for (const exact of ['same', 'same-version', 'replaced', 'missing']) {
  test(`startup recovery shares one typed result per snapshot: ${exact}`, async (t) => {
    const scanned = authoritySnapshot();
    const replacement = authoritySnapshot('version-b');
    if (exact === 'same-version') replacement.storeVersion = scanned.storeVersion;
    else replacement.objectGeneration = 18n;
    const originalDecode = publication.decodeActorAuthorityIdentity;
    const decoded = [];
    publication.decodeActorAuthorityIdentity = (...args) => {
      decoded.push(args[1]);
      return originalDecode(...args);
    };
    t.after(() => { publication.decodeActorAuthorityIdentity = originalDecode; });
    let reads = 0;
    const recovered = [];
    const runtime = new ZLinkStatefulAuthorityRouteRuntime({
      store: {
        async listAuthorities() { return { kind: 'page', items: [{ key: { value: 'actor:actor-a' }, snapshot: scanned }] }; },
        async readAuthority() {
          reads += 1;
          return exact === 'missing' ? { kind: 'missing' } : exact === 'same' ? scanned : replacement;
        }
      },
      meshNodes: new Map(), pageSize: 10,
      reportError(error) { throw error; },
      async recoverActor(snapshot) { recovered.push(snapshot); }
    });
    await runtime.start();
    assert.equal(reads, 1);
    assert.deepEqual(decoded, exact === 'replaced' ? [17n, 18n] : [17n]);
    assert.deepEqual(recovered, exact === 'missing' ? [] : [exact === 'same' ? scanned : replacement]);
  });
}

test('heartbeat submissions share protocol frames across sessions and retain their bytes after close', async () => {
  const submitted = [];
  const bytes = [];
  const socket = {
    async submit(routingId, frame) {
      submitted.push({ routingId, frame });
      bytes.push(Buffer.from(frame.data()));
      await Promise.resolve();
    }
  };
  const first = new ZLinkManagedStream(socket, 'session-a');
  const second = new ZLinkManagedStream(socket, 'session-b');
  for (const name of [protocol.ZLINK_STREAM_HEARTBEAT_PING, protocol.ZLINK_STREAM_HEARTBEAT_PONG]) {
    await Promise.all([first.writeControl(name), second.writeControl(name)]);
    const [a, b] = submitted.splice(0);
    const [beforeA, beforeB] = bytes.splice(0);
    assert.equal(a.frame, b.frame);
    assert.equal(a.frame, protocol.streamControlFrame(name));
    assert.deepEqual(a.frame.data(), beforeA);
    assert.deepEqual(b.frame.data(), beforeB);
    const decoded = protocol.tryDecodeStreamFrame(a.frame.data());
    assert.equal(protocol.decodeStreamHeader(decoded.header).name, name);
    assert.equal(decoded.payload.byteLength, 0);
  }
  assert.notEqual(protocol.streamControlFrame(protocol.ZLINK_STREAM_HEARTBEAT_PING),
    protocol.streamControlFrame(protocol.ZLINK_STREAM_HEARTBEAT_PONG));
});

test('readable poller owns readiness, internal wake and stop completion without retaining listeners', async () => {
  const factory = new ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const adapter = factory.createStreamAdapter();
  const socket = adapter.createStreamSocket(context);
  const eventLoop = nodeEventLoopPollerOf(socket);
  const setReadableHandler = eventLoop.setReadableHandler.bind(eventLoop);
  let readableHandler;
  eventLoop.setReadableHandler = (handler) => {
    readableHandler = handler;
    setReadableHandler(handler);
  };
  const poller = adapter.createReadablePoller(socket);
  const signal = new AbortController().signal;
  const ready = () => readableHandler(true, false, false);
  const listeners = () => getEventListeners(signal, 'abort').length;
  try {
    poller.wake();
    let waiting = poller.waitForReadable(signal);
    assert.equal(waiting, poller.waitForReadable(signal));
    assert.equal(listeners(), 1);
    ready();
    poller.wake();
    assert.equal(await waiting, true);
    assert.equal(poller.wait(0), true);
    assert.equal(listeners(), 0);

    poller.markDrained();
    waiting = poller.waitForReadable(signal);
    assert.equal(listeners(), 1);
    poller.wake();
    ready();
    assert.equal(await waiting, false);
    assert.equal(listeners(), 0);
    assert.equal(await poller.waitForReadable(signal), true);

    poller.markDrained();
    const stop = new AbortController();
    waiting = poller.waitForReadable(stop.signal);
    stop.abort(new Error('framework stop'));
    poller.wake();
    assert.equal(await waiting, false);
    assert.equal(getEventListeners(stop.signal, 'abort').length, 0);
    assert.equal(await poller.waitForReadable(stop.signal), false);

    waiting = poller.waitForReadable(signal);
    poller.dispose();
    assert.equal(await waiting, false);
    assert.equal(listeners(), 0);
    assert.equal(await poller.waitForReadable(signal), false);
    poller.wake();
  } finally {
    poller.dispose();
    await socket.dispose();
    await context.dispose();
  }
});
