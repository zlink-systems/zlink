const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { forwardEncodedActorPacket } = require('../../packages/framework/dist/runtime/actors/actor-client');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const { Message, RequestResult } = require('@zlink-systems/zlink');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const { ZLinkAbortError } = require('../../packages/framework/dist/runtime/abort');
const { createHook } = require('node:async_hooks');
const { waitActorReply } = require('../../packages/framework/dist/runtime/actors/actor-request-deadline');

class ActorNotify { constructor(value) { this.value = value; } }
class ActorAsk { constructor(value) { this.value = value; } }

function actorRef(actorId = 'actor-1', generation = 1n) {
  return { nodeRid: 'node-a', actorId, objectGeneration: generation, meshName: 'play-mesh' };
}

function createReplyParts(value) {
  return [
    Message.from(Buffer.from(framework.encodeStreamHeader({
      kind: framework.ZLinkStreamMessageKind.Response,
      codec: framework.ZLinkStreamCodec.Json,
      flags: framework.ZLinkStreamHeaderFlags.None,
      name: 'ActorReply',
      metadata: new Map()
    }))),
    Message.from(Buffer.from(JSON.stringify(value)))
  ];
}

function createReplyFrame(value) {
  return [
    Message.from(Buffer.from(framework.encodeStreamFrame({
      kind: framework.ZLinkStreamMessageKind.Response,
      codec: framework.ZLinkStreamCodec.Json,
      flags: framework.ZLinkStreamHeaderFlags.None,
      name: 'ActorReply',
      metadata: new Map()
    }, Buffer.from(JSON.stringify(value)))))
  ];
}

function actorLocation(actorId = 'actor-1', generation = 1n, meshName = 'play-mesh') {
  const actor = actorRef(actorId, generation);
  return {
    meshName,
    actorId,
    actorType: 'Player',
    actorRef: actor,
    ownerNodeRid: actor.nodeRid,
    ownerNodeGeneration: 7n,
    spotKind: framework.ZLinkSpotKind.Entry,
    spotId: 'node-a-entry-test',
    spotGeneration: 1n,
    membershipEpoch: 1n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 3n,
    authorityOwnerGeneration: 4n,
    authorityStoreVersion: 'store-version-1',
    updatedAt: new Date()
  };
}

function createResolver(resolve = ({ actorId }) => actorLocation(actorId)) {
  return {
    async resolveDirectActorRoute(actorId, signal, deadlineMs) {
      // This adapter has no cache: its complete resolution is an external wait.
      const route = await waitActorReply(
        Promise.resolve().then(() => resolve({ actorId }, signal)), actorId, deadlineMs, signal
      );
      return route === undefined
        ? { kind: 'missing' }
        : { kind: 'ready', route };
    },
    invalidateActorRoute() {}
  };
}

function createStoreResolver(authority, remainingLeaseMs) {
  const unusedStore = {};
  return new framework.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: { async readAuthority() { return authority; } },
      locationStore: unusedStore,
      peerStore: unusedStore,
      spotStore: unusedStore,
      actorStore: unusedStore,
      routeStore: unusedStore
    },
    leaseTracker: {
      async remainingOwnerTokenLeaseMs() { return remainingLeaseMs; }
    }
  });
}

test('cached actor request allocates no deadline timer or race promise', async t => {
  const resolver = createStoreResolver({
    kind: 'snapshot', allocation: {
      objectKind: 'actor', state: 'active', stableType: 'Player',
      descriptor: { meshName: 'play-mesh', rid: 'node-a' }, descriptorLifecycleGeneration: 7n
    }, payload: Buffer.alloc(0), objectGeneration: 1n, ownerId: 'owner-a',
    ownerLeaseGeneration: 3n, authorityOwnerGeneration: 4n, storeVersion: { value: 'v1' }
  }, 10000);
  assert.equal((await resolver.resolveDirectActorRoute('actor-1')).kind, 'ready');
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { return operationId; } }),
    completionTableProvider: () => completionTable(RequestResult.Ok, createReplyParts('pong')),
    locationResolver: () => resolver
  });
  let timers = 0;
  let promises = 0;
  const hook = createHook({ init(_id, type) {
    if (type === 'Timeout') timers++;
    if (type === 'PROMISE') promises++;
  } });
  const race = t.mock.method(Promise, 'race');
  hook.enable();
  try {
    for (let index = 0; index < 100; index++) {
      assert.equal(await client.requestToActor('actor-1', new ActorAsk('ping')).timeout(1000).submit(), 'pong');
    }
  } finally { hook.disable(); }
  console.log(`cached_actor_requests=100 timers=${timers} races=${race.mock.callCount()} promises=${promises}`);
  assert.equal(timers, 0);
  assert.equal(race.mock.callCount(), 0);
});

const operationId = Object.freeze({ high: 1n, low: 2n });
function createActorClient(options) {
  return new framework.DefaultZLinkActorClient(options);
}

function completionTable(terminalResult, parts = []) {
  return {
    async submit(operation) {
      const actualOperationId = operation();
      assert.deepEqual(actualOperationId, operationId);
      return {
        terminalResult,
        failureErrno: 0,
        operationKind: 0,
        kindData: null,
        parts
      };
    }
  };
}

test('actor client submit completes without exposing an admission result', async () => {
  const sends = [];
  const node = {
    sendToActor(actor, parts) {
      sends.push({ actor, parts });
      return 0;
    }
  };
  const resolver = createResolver();
  const client = createActorClient({
    nodeProvider: () => node,
    locationResolver: () => resolver
  });

  const call = client.sendToActor(
    'actor-1',
    new ActorNotify('ping')
  );
  const submitted = await call.submit();

  assert.equal(submitted, undefined);
  assert.equal(sends.length, 1);
  assert.equal(sends[0].actor.actorId, 'actor-1');
  assert.equal(sends[0].parts.length, 2);
  await assert.rejects(() => call.submit(), (error) => {
    assert.equal(error instanceof framework.ZLinkFrameworkException, true);
    assert.equal(error.kind, framework.ZLinkFrameworkErrorKind.InvalidOperation);
    return true;
  });
  assert.equal(sends.length, 1);
});

test('actor one-way NOT_ADMITTED completes with Rejected for returned and thrown binding results', async () => {
  const { ZLinkBackendResultError } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  for (const sendToActor of [
    () => SubmitResult.NotAdmitted,
    () => { throw new ZLinkBackendResultError('submit', SubmitResult.NotAdmitted, 0); }
  ]) {
    const client = createActorClient({
      nodeProvider: () => ({ sendToActor }),
      locationResolver: () => createResolver()
    });
    await assert.rejects(client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.Rejected);
  }
});

test('actor client writes the selected serializer into the packet codec header', async () => {
  const sent = [];
  const serializer = {
    serialize(value) {
      return framework.ZLinkEncodedPayload.from(Buffer.from(`packed:${value.value}`));
    },
    deserialize(payload) {
      return Buffer.from(payload.data()).toString('utf8');
    }
  };
  const messageSerializers = new framework.DefaultZLinkCodecRegistryBuilder()
    .addSerializer(
      'application/x-msgpack',
      serializer,
      (declaredType) => declaredType === ActorNotify
    )
    .registeredSerializers;
  const client = createActorClient({
    nodeProvider: () => ({
      sendToActor(_actor, parts) {
        sent.push({
          header: framework.decodeStreamHeader(parts[0]),
          payload: Buffer.from(parts[1]).toString('utf8')
        });
        return 0;
      }
    }),
    locationResolver: () => createResolver(),
    messageSerializers
  });

  await client.sendToActor('actor-1', new ActorNotify('ping')).submit();

  assert.equal(sent[0].header.codec, framework.ZLinkStreamCodec.MessagePack);
  assert.equal(sent[0].payload, 'packed:ping');
});

test('actor client rejects an incomplete authority fence before transport submission', async () => {
  let sends = 0;
  const client = createActorClient({
    nodeProvider: () => ({
      sendToActor() {
        sends += 1;
        return 0;
      }
    }),
    locationResolver: () => createResolver(({ actorId }) => ({
      ...actorLocation(actorId),
      ownerLeaseGeneration: 0n
    }))
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(sends, 0);
});

test('actor client selects the MeshNode and completion table from the current Actor authority', async () => {
  const selected = [];
  const nodes = new Map([
    ['mesh-a', {
      requestToActor() {
        selected.push('node:mesh-a');
        return operationId;
      }
    }],
    ['mesh-b', {
      requestToActor() {
        selected.push('node:mesh-b');
        return operationId;
      }
    }]
  ]);
  const completions = new Map([
    ['mesh-a', {
      async submit(operation) {
        assert.deepEqual(operation(), operationId);
        selected.push('completion:mesh-a');
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          operationKind: 0,
          kindData: null,
          parts: createReplyParts({ mesh: 'mesh-a' })
        };
      }
    }],
    ['mesh-b', {
      async submit(operation) {
        assert.deepEqual(operation(), operationId);
        selected.push('completion:mesh-b');
        return {
          terminalResult: RequestResult.Ok,
          failureErrno: 0,
          operationKind: 0,
          kindData: null,
          parts: createReplyParts({ mesh: 'mesh-b' })
        };
      }
    }]
  ]);
  const client = createActorClient({
    nodeProvider: (meshName) => nodes.get(meshName),
    completionTableProvider: (meshName) => completions.get(meshName),
    locationResolver: () => createResolver(({ actorId }) =>
      actorLocation(actorId, 1n, actorId === 'actor-a' ? 'mesh-a' : 'mesh-b'))
  });

  const first = await client
    .requestToActor('actor-a', new ActorAsk('ping'))
    .submit();
  const second = await client
    .requestToActor('actor-b', new ActorAsk('ping'))
    .submit();

  assert.deepEqual(first, { mesh: 'mesh-a' });
  assert.deepEqual(second, { mesh: 'mesh-b' });
  assert.deepEqual(selected, [
    'node:mesh-a',
    'completion:mesh-a',
    'node:mesh-b',
    'completion:mesh-b'
  ]);
  assert.throws(
    () => client.requestToActor('', new ActorAsk('ping')),
    /Actor ID must contain 1\.\.255 UTF-8 bytes/
  );
  assert.throws(
    () => client.requestToActor('가'.repeat(86), new ActorAsk('ping')),
    /Actor ID must contain 1\.\.255 UTF-8 bytes/
  );
});

test('actor client request decodes the handler reply and never auto-creates a missing actor', async () => {
  const node = {
    createActor() {
      throw new Error('actor client must not auto-create actors');
    },
    requestToActor(actor, parts, options) {
      assert.equal(actor.actorId, 'actor-1');
      assert.equal(parts.length, 2);
      assert.ok(options.timeoutMs > 0 && options.timeoutMs <= 100);
      return operationId;
    }
  };
  const completions = completionTable(
    RequestResult.Ok,
    createReplyParts({ value: 'pong' })
  );
  const resolver = createResolver();
  const client = createActorClient({
    nodeProvider: () => node,
    completionTableProvider: () => completions,
    locationResolver: () => resolver
  });

  const reply = await client.requestToActor('actor-1', new ActorAsk('ping'))
    .timeout(100)
    .submit();

  assert.deepEqual(reply, { value: 'pong' });
});

for (const jumpMs of [10_000, -10_000]) {
  test(`actor request keeps its local timeout across a ${jumpMs}ms wall-clock jump during resolution`, async (t) => {
    const wallNow = Date.now();
    let offsetMs = 0;
    t.mock.method(Date, 'now', () => wallNow + offsetMs);
    const client = createActorClient({
      nodeProvider: () => ({
        requestToActor(_actor, _parts, options) {
          assert.ok(options.timeoutMs > 0 && options.timeoutMs <= 100);
          assert.ok(Number.isSafeInteger(options.timeoutMs));
          return operationId;
        }
      }),
      completionTableProvider: () => completionTable(RequestResult.Ok, createReplyParts('pong')),
      locationResolver: () => createResolver(() => {
        offsetMs = jumpMs;
        return actorLocation();
      })
    });

    assert.equal(await client.requestToActor('actor-1', new ActorAsk('ping')).timeout(100).submit(), 'pong');
  });
}

test('actor client request decodes a single framed handler reply through stream protocol', async () => {
  const node = {
    requestToActor() {
      return operationId;
    }
  };
  const completions = completionTable(
    RequestResult.Ok,
    createReplyFrame({ value: 'framed-pong' })
  );
  const client = createActorClient({
    nodeProvider: () => node,
    completionTableProvider: () => completions,
    locationResolver: () => createResolver()
  });

  const reply = await client.requestToActor('actor-1', new ActorAsk('ping'))
    .submit();

  assert.deepEqual(reply, { value: 'framed-pong' });
});

test('actor handoff reply uses the original packet JSON reply contract', async () => {
  class HandoffRequest {}
  framework.ZLinkPacket('HandoffRequest', {
    payload: { type: 'object', required: [], properties: {} },
    reply: {
      type: 'object',
      required: ['generation'],
      properties: { generation: { type: 'uint64' } }
    }
  })(HandoffRequest);
  const header = Buffer.from(framework.encodeStreamHeader({
    kind: framework.ZLinkStreamMessageKind.Request,
    codec: framework.ZLinkStreamCodec.Json,
    flags: framework.ZLinkStreamHeaderFlags.None,
    name: 'HandoffRequest',
    metadata: new Map()
  }));
  const replyParts = createReplyParts({ generation: '18446744073709551615' });
  const reply = await forwardEncodedActorPacket(
    { requestToActor: () => operationId },
    completionTable(RequestResult.Ok, replyParts),
    actorRef(),
    header,
    Buffer.from('{}'),
    true,
    100
  );

  assert.equal(reply.generation, 18446744073709551615n);
});

test('actor client invalidates a stale resolved route without retrying the operation', async () => {
  const first = actorRef('actor-1', 1n);
  const sends = [];
  const node = {
    sendToActor(actor) {
      sends.push(actor);
      throw framework.createInternalFrameworkException(
        framework.ZLinkFrameworkInternalErrorKind.ActorLocationStale,
        'stale'
      );
    }
  };
  let invalidations = 0;
  const resolver = createResolver(() => actorLocation('actor-1', 1n));
  resolver.invalidateActorRoute = () => { invalidations += 1; };
  const client = createActorClient({
    nodeProvider: () => node,
    locationResolver: () => resolver
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  assert.deepEqual(sends.map((actor) => actor.generation), [1n]);
  assert.equal(invalidations, 1);
});

test('actor client submit maps native terminal outcomes to operation-specific errors', async () => {
  const results = [
    [SubmitResult.NotConnected, framework.ZLinkFrameworkErrorKind.Unavailable],
    [SubmitResult.NotFound, framework.ZLinkFrameworkErrorKind.NotFound],
    [SubmitResult.Terminated, framework.ZLinkFrameworkErrorKind.ShuttingDown],
    [SubmitResult.Backpressured, framework.ZLinkFrameworkErrorKind.Unavailable],
    [SubmitResult.NotAdmitted, framework.ZLinkFrameworkErrorKind.Rejected]
  ];
  const accepted = createActorClient({
    nodeProvider: () => ({ sendToActor: () => 0 }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver()
  });
  assert.equal(
    await accepted.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    undefined
  );

  for (const [nativeResult, expectedKind] of results) {
    const client = createActorClient({
      nodeProvider: () => ({
        sendToActor() {
          return nativeResult;
        }
      }),
      completionTableProvider: () => undefined,
      locationResolver: () => createResolver()
    });
    await assert.rejects(
      () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === expectedKind
    );
  }

  const invalid = createActorClient({
    nodeProvider: () => ({
      sendToActor() {
        return 6;
      }
    }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver()
  });
  await assert.rejects(
    () => invalid.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    /submit result 6/
  );
});

test('Actor send exposes no cancellation and keeps stale authority validation', async () => {
  const controller = new AbortController();
  controller.abort();
  let attempts = 0;
  const client = createActorClient({
    nodeProvider: () => ({
      sendToActor() {
        attempts += 1;
        return 0;
      }
    }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    staleActorRefPredicate: () => true
  });

  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(attempts, 0);
});

test('actor client maps stale and disconnected route failures', async () => {
  const noNode = createActorClient({
    nodeProvider: () => undefined,
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver()
  });
  await assert.rejects(
    () => noNode.requestToActor('missing', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  const staleNode = {
    requestToActor() {
      return operationId;
    }
  };
  const stale = createActorClient({
    nodeProvider: () => staleNode,
    completionTableProvider: () => completionTable(RequestResult.Conflict),
    locationResolver: () => createResolver(({ actorId }) => actorLocation(actorId, 1n))
  });
  await assert.rejects(
    () => stale.requestToActor('actor-1', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );

  const disconnectedNode = {
    requestToActor() {
      return operationId;
    }
  };
  const disconnected = createActorClient({
    nodeProvider: () => disconnectedNode,
    completionTableProvider: () => completionTable(RequestResult.NotConnected),
    locationResolver: () => createResolver()
  });
  await assert.rejects(
    () => disconnected.requestToActor('actor-1', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      && !('isRetriable' in error)
  );
});

test('actor client preserves ActorRouteNotFound for a missing actor route', async () => {
  const missingNode = {
    requestToActor() {
      return operationId;
    }
  };
  const client = createActorClient({
    nodeProvider: () => missingNode,
    completionTableProvider: () => completionTable(RequestResult.NotFound),
    locationResolver: () => createResolver(({ actorId }) => actorLocation(actorId))
  });

  await assert.rejects(
    () => client.requestToActor('missing-actor', new ActorAsk('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
  );
});

test('direct actor resolver maps an active authority with an expired owner lease to Unavailable', async () => {
  const resolver = createStoreResolver({
    kind: 'snapshot',
    allocation: { state: 'active' },
    payload: Buffer.alloc(0),
    objectGeneration: 1n,
    ownerId: 'owner-a',
    ownerLeaseGeneration: 3n,
    authorityOwnerGeneration: 7n
  }, 0);

  const previousDebug = process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
  const originalError = console.error;
  const observations = [];
  process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = '1';
  console.error = (...args) => observations.push(args);
  let resolution;
  try {
    resolution = await resolver.resolveDirectActorRoute('actor-1');
  } finally {
    console.error = originalError;
    if (previousDebug === undefined) {
      delete process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION;
    } else {
      process.env.ZLINK_DEBUG_FRAMEWORK_RELOCATION = previousDebug;
    }
  }
  assert.deepEqual(resolution, {
    kind: 'owner_unavailable',
    authorityGeneration: 7n,
    remainingLeaseMs: 0
  });
  assert.deepEqual(observations, [[
    '[zlink.runtime.relocation]',
    'actor_route.owner_lease_observed',
    {
      actorId: 'actor-1',
      authorityGeneration: 7n,
      remainingLeaseMs: 0,
      decision: 'owner_unavailable'
    }
  ]]);

  const client = createActorClient({
    nodeProvider: () => ({ sendToActor() { throw new Error('must not submit'); } }),
    locationResolver: () => resolver
  });
  await assert.rejects(
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      && framework.internalFrameworkErrorKind(error)
        === framework.ZLinkFrameworkInternalErrorKind.ActorRouteUnavailable
  );
});

test('direct actor resolver maps a missing authority snapshot to NotFound', async () => {
  const resolver = createStoreResolver({ kind: 'missing' }, 0);

  assert.deepEqual(await resolver.resolveDirectActorRoute('missing-actor'), {
    kind: 'missing'
  });

  const client = createActorClient({
    nodeProvider: () => ({ sendToActor() { throw new Error('must not submit'); } }),
    locationResolver: () => resolver
  });
  await assert.rejects(
    () => client.sendToActor('missing-actor', new ActorNotify('ping')).submit(),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.NotFound
      && framework.internalFrameworkErrorKind(error)
        === framework.ZLinkFrameworkInternalErrorKind.ActorRouteNotFound
  );
});

test('actor request deadline ends the caller wait before a slow route returns', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  let resolveRoute;
  const route = new Promise(resolve => { resolveRoute = resolve; });
  let submits = 0;
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { submits++; return operationId; } }),
    completionTableProvider: () => completionTable(RequestResult.Ok, createReplyParts('pong')),
    locationResolver: () => createResolver(() => route)
  });
  let terminal;
  const request = client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit();
  const observed = request.then(
    value => { terminal = value; },
    error => { terminal = error; }
  );
  try {
    t.mock.timers.tick(20);
    await new Promise(setImmediate);
    assert.equal(terminal?.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
    assert.equal(submits, 0);
    resolveRoute(actorLocation());
    await observed;
    await new Promise(setImmediate);
    assert.equal(submits, 0, 'late route completion cannot submit a timed-out request');
  } finally {
    resolveRoute(actorLocation());
    await observed;
  }
});

test('expired actor resolution observes a late Store rejection without submission', async t => {
  let now = 0;
  t.mock.method(performance, 'now', () => now);
  let rejectStore;
  const blocked = new Promise((_resolve, reject) => { rejectStore = reject; });
  const resolver = new framework.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: { readAuthority() { now = 100; return blocked; } },
      locationStore: {}, peerStore: {}, spotStore: {}, actorStore: {}, routeStore: {}
    }, leaseTracker: {}
  });
  let submits = 0;
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { submits++; return operationId; } }),
    completionTableProvider: () => undefined, locationResolver: () => resolver
  });
  await assert.rejects(
    client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(),
    error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  rejectStore(new Error('late Store failure'));
  await new Promise(setImmediate);
  assert.equal(submits, 0);
});

test('Store abort during actor resolution observes its rejected result', async () => {
  const controller = new AbortController();
  const resolver = new framework.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: { readAuthority() {
        controller.abort();
        return Promise.reject(new Error('Store cancelled'));
      } },
      locationStore: {}, peerStore: {}, spotStore: {}, actorStore: {}, routeStore: {}
    }, leaseTracker: {}
  });
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { assert.fail('aborted Store cannot submit'); } }),
    completionTableProvider: () => undefined, locationResolver: () => resolver
  });
  await assert.rejects(client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(controller.signal), ZLinkAbortError);
  await new Promise(setImmediate);
  assert.equal(require('node:events').getEventListeners(controller.signal, 'abort').length, 0);
});

test('actor handoff expiry uses the request DeadlineExceeded terminal kind', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  let reply;
  const captured = new Promise(resolve => { reply = resolve; });
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { assert.fail('captured handoff must not submit'); } }),
    completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    handoffCapture: () => captured
  });
  const request = client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit();
  const terminal = assert.rejects(request, error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  try {
    await new Promise(setImmediate);
    t.mock.timers.tick(20);
    await terminal;
  } finally {
    reply('late');
  }
});

for (const stage of ['route', 'handoff', 'native']) {
  test(`actor request cancellation ends the ${stage} wait and removes its listener`, async t => {
    t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
    const { getEventListeners } = require('node:events');
    const controller = new AbortController();
    let release;
    const blocked = new Promise(resolve => { release = resolve; });
    let started;
    const entered = new Promise(resolve => { started = resolve; });
    let submits = 0;
    let routeSignal;
    let nativeSignal;
    const nativeTable = new (require('../../packages/framework/dist/runtime/backend').ZLinkMeshCompletionTable)();
    const nativeSubmit = nativeTable.submit.bind(nativeTable);
    nativeTable.submit = (operation, signal) => {
      nativeSignal = signal;
      const result = nativeSubmit(operation, signal);
      started();
      return result;
    };
    const client = createActorClient({
      nodeProvider: () => ({ requestToActor() { submits++; return operationId; } }),
      completionTableProvider: () => nativeTable,
      locationResolver: () => createResolver((_actor, signal) => {
        routeSignal = signal;
        if (stage === 'route') { started(); return blocked; }
        return actorLocation();
      }),
      handoffCapture: stage === 'handoff' ? () => { started(); return blocked; } : undefined
    });
    const scheduled = t.mock.method(globalThis, 'setTimeout');
    const clear = t.mock.method(globalThis, 'clearTimeout');
    const request = client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(controller.signal);
    const terminal = assert.rejects(request, error => stage === 'native' ? error === controller.signal.reason : error instanceof ZLinkAbortError);
    await entered;
    controller.abort();
    await terminal;
    assert.equal(routeSignal, controller.signal);
    if (stage === 'native') assert.equal(nativeSignal, controller.signal);
    assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
    assert.deepEqual(
      clear.mock.calls.map(call => call.arguments[0]),
      scheduled.mock.calls.map(call => call.result),
      'every actual wait releases its deadline timer'
    );
    if (stage === 'route') {
      release(actorLocation());
      await new Promise(setImmediate);
      assert.equal(submits, 0);
    } else if (stage === 'native') {
      nativeTable.complete({ operationId, terminalResult: require('../../packages/framework/dist/runtime/backend/runtime-values').RequestResult.TimedOut, failureErrno: 0, parts: [] });
      assert.equal(nativeTable.pendingCount, 0);
    } else release('late');
    t.mock.timers.tick(20);
    await new Promise(setImmediate);
  });
}

test('actor request without a deadline remains cancellable during handoff', async () => {
  const controller = new AbortController();
  let release;
  const blocked = new Promise(resolve => { release = resolve; });
  let started;
  const entered = new Promise(resolve => { started = resolve; });
  const client = createActorClient({
    nodeProvider: () => undefined, completionTableProvider: () => undefined,
    locationResolver: () => createResolver(),
    handoffCapture: () => { started(); return blocked; }
  });
  const terminal = assert.rejects(client.requestToActor('actor-1', new ActorAsk('ping')).submit(controller.signal), ZLinkAbortError);
  await entered;
  controller.abort();
  await terminal;
  release('late');
});

test('actor request clears its sole deadline timer after route failure and success', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  const clear = t.mock.method(globalThis, 'clearTimeout');
  const cause = new Error('route failed');
  const failed = createActorClient({
    nodeProvider: () => undefined, completionTableProvider: () => undefined,
    locationResolver: () => createResolver(() => { throw cause; })
  });
  await assert.rejects(failed.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(), error => error === cause);
  assert.equal(clear.mock.callCount(), 1);
  const ready = createActorClient({
    nodeProvider: () => ({ requestToActor() { return operationId; } }),
    completionTableProvider: () => completionTable(RequestResult.Ok, createReplyParts('pong')),
    locationResolver: () => createResolver()
  });
  assert.equal(await ready.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(), 'pong');
  assert.equal(clear.mock.callCount(), 2);
  t.mock.timers.tick(20);
  await new Promise(setImmediate);
});


test('actor native deadline preserves one terminal and releases the lower completion listener', async t => {
  t.mock.timers.enable({ apis: ['setTimeout', 'Date'], now: 0 });
  t.mock.method(performance, 'now', () => Date.now());
  const { getEventListeners } = require('node:events');
  const backend = require('../../packages/framework/dist/runtime/backend');
  const values = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const controller = new AbortController();
  const table = new backend.ZLinkMeshCompletionTable();
  let entered;
  const started = new Promise(resolve => { entered = resolve; });
  const submit = table.submit.bind(table);
  table.submit = (operation, signal) => {
    const result = submit(operation, signal);
    entered();
    return result;
  };
  const client = createActorClient({
    nodeProvider: () => ({ requestToActor() { return operationId; } }),
    completionTableProvider: () => table,
    locationResolver: () => createResolver()
  });
  const terminal = assert.rejects(client.requestToActor('actor-1', new ActorAsk('ping')).timeout(20).submit(controller.signal),
    error => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
  await started;
  t.mock.timers.tick(20);
  table.complete({ operationId, terminalResult: values.RequestResult.TimedOut, failureErrno: 0, parts: [] });
  await terminal;
  await new Promise(setImmediate);
  assert.equal(table.pendingCount, 0);
  assert.equal(getEventListeners(controller.signal, 'abort').length, 0);
  table.complete({ operationId, terminalResult: values.RequestResult.Ok, failureErrno: 0, parts: [] });
  assert.equal(table.pendingCount, 0);
  table.dispose();
});

for (const [phase, expectedKind] of [
  ['submit', framework.ZLinkFrameworkErrorKind.Unavailable],
  ['completion', framework.ZLinkFrameworkErrorKind.DeadlineExceeded]
]) {
  test(`Actor capacity ${phase} preserves its public error kind`, async () => {
    const { SubmitResult } = require('@zlink-systems/zlink');
    const {
      ZLinkBackendResultError
    } = require('../../packages/framework/dist/runtime/backend/runtime-values');
    const client = createActorClient({
      nodeProvider: () => ({
        sendToActor() {
          throw new ZLinkBackendResultError('submit', SubmitResult.Backpressured, undefined, {
            phase
          });
        }
      }),
      completionTableProvider: () => undefined,
      locationResolver: () => createResolver()
    });
    await assert.rejects(
      () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === expectedKind
    );
  });
}
