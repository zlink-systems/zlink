const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const { forwardEncodedActorPacket } = require('../../packages/framework/dist/runtime/actors/actor-client');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const { Message, RequestResult } = require('@zlink-systems/zlink');

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
    async resolveDirectActorRoute(actorId, signal) {
      const route = await resolve({ actorId }, signal);
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
    [2, framework.ZLinkFrameworkErrorKind.Unavailable],
    [3, framework.ZLinkFrameworkErrorKind.NotFound],
    [4, framework.ZLinkFrameworkErrorKind.ShuttingDown]
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

  for (const nativeResult of [1, 13]) {
    const client = createActorClient({
      nodeProvider: () => ({ sendToActor: () => nativeResult }),
      completionTableProvider: () => undefined,
      locationResolver: () => createResolver()
    });
    await assert.rejects(
      () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(),
      (error) => error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
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

test('pre-aborted Actor call does not read authority or submit transport work', async () => {
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
    () => client.sendToActor('actor-1', new ActorNotify('ping')).submit(controller.signal),
    (error) => error?.name === 'AbortError'
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
