const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const {
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

function actorRow(actorId, nodeRid = 'node-a') {
  return {
    meshName: 'play',
    actorId,
    actorType: 'Player',
    actorRef: { nodeRid, actorId, objectGeneration: 1n },
    ownerNodeRid: nodeRid,
    ownerNodeGeneration: 1n,
    spotKind: framework.ZLinkSpotKind.Entry,
    spotId: `${nodeRid}-entry-test`,
    spotGeneration: 1n,
    membershipEpoch: 1n,
    ownerId: 'owner-a',
    leaseGeneration: 2n,
    updatedAt: new Date()
  };
}

function resolverFor(resolveActor, now) {
  const unused = () => { throw new Error('unexpected store operation'); };
  return new internal.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: { readAuthority: unused },
      locationStore: { listMeshNodes: unused },
      peerStore: { listPeers: unused },
      spotStore: { resolveSpot: unused },
      actorStore: { resolveActor },
      routeStore: { resolveRoute: unused }
    },
    leaseTracker: {
      isOwnerLive: async () => true,
      remainingOwnerTokenLeaseMs: async () => 60_000
    },
    spotMeshNames: ['play'],
    routeCacheMaxAgeMs: 15_000,
    monotonicNowMs: now
  });
}

test('direct Actor resolution caches only positive Ready routes and supports explicit invalidation', async () => {
  let now = 0;
  let reads = 0;
  let current = actorRow('actor-1');
  const resolver = resolverFor(async ({ actorId }) => {
    reads += 1;
    return actorId === current?.actorId ? current : undefined;
  }, () => now);

  assert.equal((await resolver.resolveActorRow({ meshName: '', actorId: 'actor-1' })).actorRef.nodeRid, 'node-a');
  current = actorRow('actor-1', 'node-b');
  assert.equal((await resolver.resolveActorRow({ meshName: '', actorId: 'actor-1' })).actorRef.nodeRid, 'node-a');
  assert.equal(reads, 1);

  resolver.invalidateActorRoute('actor-1');
  assert.equal((await resolver.resolveActorRow({ meshName: '', actorId: 'actor-1' })).actorRef.nodeRid, 'node-b');
  assert.equal(reads, 2);

  now = 15_000;
  current = actorRow('actor-1', 'node-c');
  assert.equal((await resolver.resolveActorRow({ meshName: '', actorId: 'actor-1' })).actorRef.nodeRid, 'node-c');
  assert.equal(reads, 3);
});

test('direct Actor resolution does not negative-cache Missing results', async () => {
  let reads = 0;
  const resolver = resolverFor(async () => {
    reads += 1;
    return undefined;
  }, () => 0);

  assert.equal(await resolver.resolveActorRow({ meshName: '', actorId: 'missing' }), undefined);
  assert.equal(await resolver.resolveActorRow({ meshName: '', actorId: 'missing' }), undefined);
  assert.equal(reads, 2);
});

test('authority-backed Spot resolution uses the same positive-only cache boundary', async () => {
  let reads = 0;
  const authority = {
    async readAuthority() {
      reads += 1;
      return {
        kind: 'snapshot',
        storeVersion: { value: '1' },
        payload: internal.encodeServiceUserSpotAuthorityPayload({
          state: 'ready',
          stableType: 'Room',
          spotId: 'room-1',
          ownerId: 'owner-a',
          ownerLeaseGeneration: 2n,
          ownerMeshName: 'play',
          ownerNodeRid: 'node-a',
          ownerNodeGeneration: 3n
        }),
        objectGeneration: 4n,
        authorityOwnerGeneration: 5n,
        ownerId: 'owner-a',
        ownerLeaseGeneration: 2n,
        allocation: {
          state: 'active',
          objectKind: 'user_spot',
          stableType: 'Room',
          descriptor: { meshName: 'play', nodeRid: 'node-a' },
          descriptorLifecycleGeneration: 3n,
          capacity: { actors: 0, spots: 1 }
        },
        storeNow: new Date()
      };
    }
  };
  const resolver = new internal.ZLinkAuthoritySpotRouteResolver(
    authority,
    (meshName) => meshName,
    undefined,
    { remainingOwnerTokenLeaseMs: async () => 60_000 },
    15_000,
    () => 0
  );

  assert.equal((await resolver.resolve('room-1')).targetNodeRid, 'node-a');
  assert.equal((await resolver.resolve('room-1')).authorityOwnerGeneration, 5n);
  assert.equal(reads, 1);
  resolver.invalidate('room-1');
  await resolver.resolve('room-1');
  assert.equal(reads, 2);
});

test('authority Spot resolution does not re-cache a route invalidated during the read', async () => {
  let reads = 0;
  let nodeRid = 'node-a';
  let storeVersion = 'v1';
  let releaseRead;
  let signalReadStarted;
  const readStarted = new Promise(resolve => {
    signalReadStarted = resolve;
  });
  const authority = {
    async readAuthority() {
      reads += 1;
      const readNodeRid = nodeRid;
      const readStoreVersion = storeVersion;
      if (reads === 1) {
        signalReadStarted();
        await new Promise(resolve => {
          releaseRead = resolve;
        });
      }
      return {
        kind: 'snapshot',
        storeVersion: { value: readStoreVersion },
        payload: internal.encodeServiceUserSpotAuthorityPayload({
          state: 'ready',
          stableType: 'Room',
          spotId: 'room-epoch',
          ownerId: 'owner-a',
          ownerLeaseGeneration: 2n,
          ownerMeshName: 'play',
          ownerNodeRid: readNodeRid,
          ownerNodeGeneration: 3n
        }),
        objectGeneration: 4n,
        authorityOwnerGeneration: 5n,
        ownerId: 'owner-a',
        ownerLeaseGeneration: 2n,
        allocation: {
          state: 'active',
          objectKind: 'user_spot',
          stableType: 'Room',
          descriptor: { meshName: 'play', nodeRid: readNodeRid },
          descriptorLifecycleGeneration: 3n,
          capacity: { actors: 0, spots: 1 }
        },
        storeNow: new Date()
      };
    }
  };
  const resolver = new internal.ZLinkAuthoritySpotRouteResolver(
    authority,
    (meshName) => meshName,
    undefined,
    { remainingOwnerTokenLeaseMs: async () => 60_000 },
    15_000,
    () => 0
  );

  const pending = resolver.resolve('room-epoch');
  await readStarted;
  resolver.invalidate('room-epoch');
  nodeRid = 'node-b';
  storeVersion = 'v2';
  releaseRead();

  assert.equal((await pending).targetNodeRid, 'node-a');
  assert.equal((await resolver.resolve('room-epoch')).targetNodeRid, 'node-b');
  assert.equal(reads, 2);
});

test('authority Actor route carries every fence and a changed StoreVersion invalidates it', async () => {
  let reads = 0;
  let storeVersion = 'v1';
  const authority = {
    async readAuthority() {
      reads += 1;
      return {
        kind: 'snapshot',
        storeVersion: { value: storeVersion },
        payload: internal.encodeActorAuthorityIdentity({
          actorType: 'Player',
          actor: { actorId: 'actor-1', nodeRid: 'node-a', objectGeneration: 7n },
          meshName: 'play',
          ownerNodeGeneration: 11n,
          owner: { ownerId: 'owner-a', leaseGeneration: 13n }
        }),
        objectGeneration: 7n,
        authorityOwnerGeneration: 17n,
        ownerId: 'owner-a',
        ownerLeaseGeneration: 13n,
        allocation: {
          state: 'active',
          objectKind: 'actor',
          stableType: 'Player',
          descriptor: { meshName: 'play', nodeRid: 'node-a' },
          descriptorLifecycleGeneration: 11n,
          capacity: { actors: 1, spots: 0 }
        },
        storeNow: new Date()
      };
    }
  };
  const unused = () => { throw new Error('legacy object row must not be read'); };
  const resolver = new internal.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: authority,
      locationStore: { listMeshNodes: unused },
      peerStore: { listPeers: unused },
      spotStore: { resolveSpot: unused },
      actorStore: { resolveActor: unused },
      routeStore: { resolveRoute: unused }
    },
    leaseTracker: { remainingOwnerTokenLeaseMs: async () => 60_000 },
    routeCacheMaxAgeMs: 15_000,
    monotonicNowMs: () => 0
  });

  const first = await resolver.resolveDirectActorRoute('actor-1');
  assert.equal(first.authorityStoreVersion, 'v1');
  assert.equal(first.authorityOwnerGeneration, 17n);
  assert.equal(first.ownerLeaseGeneration, 13n);
  await resolver.resolveDirectActorRoute('actor-1');
  assert.equal(reads, 1);

  storeVersion = 'v2';
  resolver.observeActorAuthorityVersion('actor-1', 'v2');
  assert.equal((await resolver.resolveDirectActorRoute('actor-1')).authorityStoreVersion, 'v2');
  assert.equal(reads, 2);
});

test('Message Follow invalidation deletes only the exact cached Actor route fence', async () => {
  let nodeRid = 'node-a';
  let nodeGeneration = 11n;
  let authorityOwnerGeneration = 17n;
  let ownerLeaseGeneration = 13n;
  const authority = {
    async readAuthority() {
      return {
        kind: 'snapshot',
        storeVersion: { value: `v-${authorityOwnerGeneration}` },
        payload: internal.encodeActorAuthorityIdentity({
          actorType: 'Player',
          actor: { actorId: 'actor-follow', nodeRid, objectGeneration: 7n },
          meshName: 'play',
          ownerNodeGeneration: nodeGeneration,
          owner: { ownerId: 'owner-a', leaseGeneration: ownerLeaseGeneration }
        }),
        objectGeneration: 7n,
        authorityOwnerGeneration,
        ownerId: 'owner-a',
        ownerLeaseGeneration,
        allocation: {
          state: 'active',
          objectKind: 'actor',
          stableType: 'Player',
          descriptor: { meshName: 'play', rid: nodeRid },
          descriptorLifecycleGeneration: nodeGeneration,
          capacity: { actors: 1, spots: 0 }
        },
        storeNow: new Date()
      };
    }
  };
  const unused = () => { throw new Error('unexpected legacy store access'); };
  const resolver = new internal.ZLinkStoreLocationResolvers({
    stores: {
      authorityStore: authority,
      locationStore: { listMeshNodes: unused },
      peerStore: { listPeers: unused },
      spotStore: { resolveSpot: unused },
      actorStore: { resolveActor: unused },
      routeStore: { resolveRoute: unused }
    },
    leaseTracker: { remainingOwnerTokenLeaseMs: async () => 60_000 },
    routeCacheMaxAgeMs: 15_000,
    monotonicNowMs: () => 0
  });
  await resolver.resolveDirectActorRoute('actor-follow');

  nodeRid = 'node-b';
  nodeGeneration = 19n;
  authorityOwnerGeneration = 23n;
  ownerLeaseGeneration = 29n;
  assert.equal(resolver.invalidateActorRouteIfMatches({
    actorId: 'actor-follow',
    objectGeneration: 7n,
    targetNodeRid: 'node-stale-not-current',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 17n,
    ownerLeaseGeneration: 13n
  }), false);
  assert.equal((await resolver.resolveDirectActorRoute('actor-follow')).actorRef.nodeRid, 'node-a');

  assert.equal(resolver.invalidateActorRouteIfMatches({
    actorId: 'actor-follow',
    objectGeneration: 7n,
    targetNodeRid: 'node-a',
    targetNodeGeneration: 11n,
    authorityOwnerGeneration: 17n,
    ownerLeaseGeneration: 13n
  }), true);
  assert.equal((await resolver.resolveDirectActorRoute('actor-follow')).actorRef.nodeRid, 'node-b');
});

test('Session Actor relay route is the stored binding route rather than a per-message Store lookup', () => {
  const bindings = new internal.ZLinkActorSessionBindingRegistry();
  const localBindings = [];
  const context = {
    bindLocal(actor, bindingToken) {
      localBindings.push({ actor, bindingToken });
    },
    unbindLocal() {}
  };
  const actor = {
    actorId: 'actor-1',
    ref: { actorId: 'actor-1', nodeRid: 'node-a', generation: 7n }
  };

  bindings.bind(context, actor, 'binding-11');

  const route = bindings.requireRoute('actor-1');
  assert.equal(route.context, context);
  assert.equal(route.actor.ref.nodeRid, 'node-a');
  assert.equal(route.bindingToken, 'binding-11');
  assert.deepEqual(localBindings, [{ actor, bindingToken: 'binding-11' }]);
});

test('Session owner replaces one stored Actor route without exposing a missing-route window', () => {
  const bindings = new internal.ZLinkActorSessionBindingRegistry();
  const events = [];
  const oldActor = {
    actorId: 'actor-atomic',
    ref: { actorId: 'actor-atomic', nodeRid: 'node-old', generation: 9n }
  };
  const newActor = {
    actorId: 'actor-atomic',
    ref: { actorId: 'actor-atomic', nodeRid: 'node-new', generation: 9n }
  };
  const oldContext = {
    bindLocal() {},
    unbindLocal(actorId, token) {
      events.push(`old-unbind:${actorId}:${token}`);
      assert.equal(bindings.requireRoute(actorId).actor, oldActor);
    }
  };
  const newContext = {
    bindLocal(actor, token) {
      events.push(`new-bind:${actor.actorId}:${token}`);
      assert.equal(bindings.requireRoute(actor.actorId).actor, oldActor);
    },
    unbindLocal() {}
  };

  bindings.bind(oldContext, oldActor, 'binding-old');
  const previous = bindings.requireRoute(oldActor.actorId);
  bindings.replace(previous, newContext, newActor, 'binding-new');

  assert.deepEqual(events, [
    'new-bind:actor-atomic:binding-new',
    'old-unbind:actor-atomic:binding-old'
  ]);
  assert.equal(bindings.requireRoute(oldActor.actorId).actor, newActor);
  bindings.unbind(oldActor.actorId, oldContext, 'binding-old');
  assert.equal(bindings.requireRoute(oldActor.actorId).actor, newActor);
});

test('request reply preserves its reply correlation without resolving the requester object route', () => {
  const request = {
    kind: internal.ZLinkStreamMessageKind.Request,
    codec: internal.ZLinkStreamCodec.Json,
    flags: internal.ZLinkStreamHeaderFlags.HasRequestSeq
      | internal.ZLinkStreamHeaderFlags.HasMetadata,
    requestSeq: 731n,
    name: 'Lookup',
    metadata: new Map([['request-only', 'value']]),
    correlationId: 'reply-route-731',
    flowId: 'flow-731',
    flowOrigin: 'Inbound'
  };

  const reply = internal.createStreamReplyHeader(
    request,
    internal.ZLinkStreamMessageKind.Response,
    internal.ZLinkStreamCodec.Json,
    internal.ZLinkStreamHeaderFlags.HasRequestSeq,
    new Map()
  );

  assert.equal(reply.requestSeq, 731n);
  assert.equal(reply.correlationId, 'reply-route-731');
  assert.equal(reply.flowId, 'flow-731');
  assert.equal(reply.flowOrigin, 'Inbound');
  assert.equal(reply.metadata.has('request-only'), false);
});

test('a failed direct Spot operation does not refresh and resubmit to a fresh owner', async () => {
  class Notice {}
  let refreshCount = 0;
  let submitCount = 0;
  const handle = internal.createSpotHandle('spot-1', {
    meshName: 'play',
    nodeRid: 'node-old',
    spotId: 'spot-1',
    spotKind: framework.ZLinkSpotKind.User
  }, async () => {
    refreshCount += 1;
    return {
      meshName: 'play',
      nodeRid: 'node-new',
      spotId: 'spot-1',
      spotKind: framework.ZLinkSpotKind.User
    };
  });

  const result = await internal.sendToSpotHandle({
    async sendToSpot() {
      submitCount += 1;
      return { status: ZLinkSubmitStatus.TargetNotFound };
    },
    async requestToSpot() {
      throw new Error('unexpected request');
    }
  }, handle, new Notice());

  assert.equal(result.status, ZLinkSubmitStatus.TargetNotFound);
  assert.equal(submitCount, 1);
  assert.equal(refreshCount, 0);
});

test('a direct Spot request maps a confirmed draining target to ShuttingDown without retrying', async () => {
  class Lookup {}
  let refreshCount = 0;
  let requestCount = 0;
  const handle = internal.createSpotHandle('spot-1', {
    meshName: 'play',
    nodeRid: 'node-a',
    spotId: 'spot-1',
    spotKind: framework.ZLinkSpotKind.User,
    targetNodeState: framework.ZLinkFrameworkRuntimeState.Serving
  }, async () => {
    refreshCount += 1;
    return {
      meshName: 'play',
      nodeRid: 'node-a',
      spotId: 'spot-1',
      spotKind: framework.ZLinkSpotKind.User,
      targetNodeState: framework.ZLinkFrameworkRuntimeState.Draining
    };
  });

  await assert.rejects(
    () => internal.requestToSpotHandle({
      async sendToSpot() {},
      async requestToSpot() {
        requestCount += 1;
        throw internal.createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          'SpotNode router request failed with result 109.'
        );
      }
    }, handle, new Lookup()),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.ShuttingDown
  );
  assert.equal(requestCount, 1);
  assert.equal(refreshCount, 1);
});

test('Spot address requests preserve the original route failure after invalidation', async () => {
  class Lookup {}
  let targetState = framework.ZLinkFrameworkRuntimeState.Serving;
  let invalidated = 0;
  const resolver = {
    async resolve() {
      return {
        routerChannelId: 'play',
        targetNodeRid: 'node-a',
        spotId: 'spot-1',
        spotKind: framework.ZLinkSpotKind.User,
        targetNodeState: targetState
      };
    },
    invalidate() {
      invalidated += 1;
      targetState = framework.ZLinkFrameworkRuntimeState.Draining;
    }
  };
  const addressTransport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => resolver,
    routed: {
      async sendToSpot() {},
      async requestToSpot() {
        throw internal.createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.RouteNotConnected,
          'MeshNode request failed with result 109.'
        );
      }
    },
    meshNames: () => [],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 1_000
  });

  await assert.rejects(
    () => addressTransport.requestToSpotAddress('spot-1', new Lookup(), {
      instanceSpot: false
    }),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  assert.equal(invalidated, 1);
});

test('Spot address deadline includes authority resolution and prevents a late submit', async () => {
  let resolveAuthority;
  let submits = 0;
  const authorityRead = new Promise((resolve) => { resolveAuthority = resolve; });
  const addressTransport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        await authorityRead;
        return {
          routerChannelId: 'play',
          targetNodeRid: 'node-a',
          spotId: 'spot-1',
          spotKind: framework.ZLinkSpotKind.User
        };
      }
    }),
    routed: {
      async sendToSpot() {
        submits += 1;
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async requestToSpot() {
        submits += 1;
        return 'unexpected';
      }
    },
    meshNames: () => [],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 20
  });

  const result = await addressTransport.sendToSpotAddress('spot-1', { value: 1 }, {
    instanceSpot: false
  });
  assert.deepEqual(result, { status: ZLinkSubmitStatus.TimedOut });
  assert.equal(submits, 0);
  resolveAuthority();
});

test('Spot one-way address uses the selected Mesh send timeout instead of request timeout', async () => {
  let releaseAuthority;
  let submits = 0;
  const authorityRead = new Promise((resolve) => { releaseAuthority = resolve; });
  const startedAt = Date.now();
  const addressTransport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        await authorityRead;
        return {
          routerChannelId: 'play',
          targetNodeRid: 'node-a',
          spotId: 'spot-1',
          spotKind: framework.ZLinkSpotKind.User
        };
      }
    }),
    routed: {
      async sendToSpot() {
        submits += 1;
        return { status: ZLinkSubmitStatus.Submitted };
      },
      async requestToSpot() {
        throw new Error('request is not used by this test.');
      }
    },
    meshNames: () => ['play'],
    meshNode: () => undefined,
    completions: () => undefined,
    defaultRequestTimeoutMs: 5_000,
    defaultSendTimeoutMs: 500,
    sendTimeoutMsForMesh: (mesh) => mesh === 'play' ? 40 : 500,
    sendTimeoutMsForRouteChannel: (route) => route === 'play' ? 40 : 500
  });

  const result = await addressTransport.sendToSpotAddress('spot-1', { value: 1 }, {
    instanceSpot: false,
    initialMeshName: 'play'
  });
  assert.deepEqual(result, { status: ZLinkSubmitStatus.TimedOut });
  assert.equal(submits, 0);
  assert.ok(Date.now() - startedAt < 500);
  releaseAuthority();
});

test('Missing Instance send waits for bounded admission before submitting once', async () => {
  class Notice {}
  let sendAttempts = 0;
  let admissionCalls = 0;
  const node = {
    instanceSpotPlacementTypes() {
      return ['chat-room'];
    },
    selectObjectPlacement() {
      return {
        kind: 'selected',
        target: {
          targetNodeRid: 'node-b',
          targetNodeGeneration: 7n,
          descriptorVersion: '11'
        }
      };
    },
    sendToMissingInstanceSpot() {
      sendAttempts += 1;
      return sendAttempts === 1 ? 1 : 0;
    }
  };
  const registry = new internal.ZLinkMeshSubmitterRegistry(100, 4);
  const addressTransport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => ({
      async resolve() {
        throw internal.createInternalFrameworkException(
          ZLinkFrameworkInternalErrorKind.SpotRouteNotFound,
          'missing'
        );
      }
    }),
    routed: {
      async sendToSpot() {
        throw new Error('Ready route must not be used.');
      },
      async requestToSpot() {
        throw new Error('Ready route must not be used.');
      }
    },
    meshNames: () => ['play'],
    meshNode: () => node,
    completions: () => undefined,
    defaultRequestTimeoutMs: 100,
    submitMissingInstance: (meshName, attempt, signal, timeoutMs) => {
      admissionCalls += 1;
      const pending = registry.submit(meshName, attempt, signal, timeoutMs);
      setTimeout(() => registry.notify(meshName), 5);
      return pending;
    }
  });

  assert.deepEqual(
    await addressTransport.sendToSpotAddress('instance-1', new Notice(), {
      instanceSpot: true,
      instanceSpotType: 'chat-room',
      initialMeshName: 'play'
    }),
    { status: ZLinkSubmitStatus.Submitted }
  );
  assert.equal(admissionCalls, 1);
  assert.equal(sendAttempts, 2);
  registry.dispose();
});
