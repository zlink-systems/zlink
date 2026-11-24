const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const internal = require('../../packages/framework/dist/internal');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const {
  ZLinkLocationStoreRepository
} = require('../../packages/framework/dist/runtime/locations/location-store-repository');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const {
  internalFrameworkErrorKind,
  ZLinkFrameworkInternalErrorKind
} = require('../../packages/framework/dist/runtime/framework-errors-internal');
const {
  ZLinkInstanceActivationAuthority
} = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/internal');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  encodeApplicationPayload,
  decodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

async function fixture(
  provider,
  targetPolicy = 'disabled',
  placementTypes = ['room', 'other-room']
) {
  let now = new Date(1000);
  const clock = { monotonicMs: 0 };
  let changeDuringRelease;
  // The in-memory release runs synchronously. Enter the descriptor write through
  // its existing release-delay clock to exercise the row fence before mutation.
  const monotonicNowMs = () => {
    const change = changeDuringRelease;
    if (!provider && change !== undefined) {
      changeDuringRelease = undefined;
      change();
    }
    return clock.monotonicMs;
  };
  let releaseWrite;
  class Provider extends ZLinkInMemoryProviderLocationStore {
    async write(request, signal) {
      if (
        changeDuringRelease !== undefined &&
        request.mutations.some(
          (mutation) => mutation.kind === 'delete' && mutation.key.value.startsWith('authority\0')
        )
      ) {
        const change = changeDuringRelease;
        changeDuringRelease = undefined;
        await change();
        releaseWrite = await super.write(request, signal);
        return releaseWrite;
      }
      return super.write(request, signal);
    }
  }
  const store = provider
    ? new ZLinkLocationStoreRepository(
        new Provider(() => now),
        () => now,
        undefined,
        monotonicNowMs,
        1
      )
    : new internal.ZLinkInMemoryLocationStore(() => now, monotonicNowMs, 1);
  async function descriptor(ownerId, rid, policy = 'disabled') {
    const lease = await store.claimOwnerLease(ownerId, rid === 'node-b' ? 60000 : 30000);
    assert.equal(lease.kind, 'claimed');
    const owner = lease.token;
    const meshNode = {
        meshName: 'mesh',
        rid,
        lifecycleGeneration: 1n,
        descriptorRevision: 1n,
        endpoint: 'tcp://node',
        objectRole: framework.ZLinkObjectRole.Server,
        placementWeight: 1,
        populationCapacity: {
          actors: { active: 0, reserved: 0, limit: 1 },
          spots: { active: 0, reserved: 0, limit: 1 },
          spotTypes: [
            { objectKind: 'instance_spot', stableType: 'room', active: 0, reserved: 0, limit: 1 }
          ]
        },
        activationConcurrency: { active: 0, limit: 1 },
        channelWeights: {},
        applicationVersion: 1n,
        spotTypes: ['room'],
        objectCapabilities: [
          {
            objectKind: 'actor',
            stableType: 'player',
            policy: 'disabled',
            hasSnapshotAdapter: false,
            limit: 0
          },
          ...(policy === null
            ? []
            : [{
                objectKind: 'instance_spot',
                stableType: 'room',
                policy,
                hasSnapshotAdapter: policy === 'snapshot',
                limit: 1
              }])
        ],
        state: framework.ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: 'test',
        ownerId,
        leaseGeneration: owner.leaseGeneration,
        updatedAt: now
      };
    const claim = () =>
      store.updateMeshNode(meshNode, internal.ZLinkInternalMeshNodeWriteIntent.FixedRidNewClaim);
    if (now.getTime() === 1000 && rid === 'node-a') {
      await assert.rejects(
        claim(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      clock.monotonicMs += 3;
    }
    if (now.getTime() > 31_000 && rid === 'node-a') {
      await assert.rejects(
        claim(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      clock.monotonicMs += 3;
    }
    if (now.getTime() > 31_000 && rid === 'node-b') {
      await assert.rejects(
        claim(),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      clock.monotonicMs += 3;
    }
    const result = await claim();
    assert.equal(result.status, internal.ZLinkLocationWriteStatus.Stored);
    return owner;
  }
  const oldOwner = await descriptor('old-owner', 'node-a');
  const oldTarget = {
    meshName: 'mesh',
    nodeRid: 'node-a',
    nodeLifecycleGeneration: 1n,
    owner: oldOwner
  };
  const key = encodeAuthorityKey('instance_spot', 'ended-room');
  const request = {
    key: { kind: 'instance_spot', globalId: 'ended-room' },
    intent: {
      stableType: 'room',
      requestContentReference: 'old-request',
      requestSha256: Buffer.alloc(32, 1),
      requestEncodedSize: 1n
    },
    target: oldTarget,
    creatingPayload: Buffer.from('creating'),
    capacity: {
      actors: 0,
      spots: 1,
      spotType: { objectKind: 'instance_spot', stableType: 'room', count: 1 }
    }
  };
  const reserved = await store.reserve(request);
  assert.equal(reserved.kind, 'reserved');
  const committed = await store.commit({
    key: request.key,
    target: oldTarget,
    reservationId: reserved.reservationId,
    expectedStoreVersion: reserved.creating.storeVersion.value,
    readyPayload: internal.encodeServiceInstanceAuthorityPayload({
      state: 'ready',
      stableType: 'room',
      spotId: 'ended-room',
      ownerId: oldOwner.ownerId,
      ownerLeaseGeneration: oldOwner.leaseGeneration,
      ownerMeshName: 'mesh',
      ownerNodeRid: 'node-a',
      ownerNodeGeneration: 1n
    })
  });
  assert.equal(committed.kind, 'committed');
  now = new Date(32000);
  const owners = {
    'node-a': await descriptor('new-owner-a', 'node-a', targetPolicy),
    'node-b': await descriptor('new-owner-b', 'node-b')
  };
  await assert.rejects(
    store.reserve({ ...request, target: { ...oldTarget, nodeRid: 'node-b', owner: owners['node-b'] } }),
    (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
  );
  clock.monotonicMs = 30_000;
  const roots = new Map();
  const relocationStore = {
    async put(reference, bytes) {
      roots.set(reference.value, Buffer.from(bytes));
      return { kind: 'stored', storeNow: now, expiresAt: new Date(now.getTime() + 60000) };
    },
    async read(reference) {
      const bytes = roots.get(reference.value);
      return bytes === undefined
        ? { kind: 'missing', storeNow: now }
        : { kind: 'found', bytes, storeNow: now, expiresAt: new Date(now.getTime() + 60000) };
    },
    async delete(reference) {
      roots.delete(reference.value);
    }
  };
  let factories = 0;
  const runtimes = [];
  function activation(rid) {
    const runtime = new ServiceStatefulRuntime(
      { setServiceIngress() {}, topology: { localDescriptor: () => undefined } },
      rid,
      1n
    );
    runtimes.push(runtime);
    const target = {
      targetSpotId: 'ended-room',
      stableType: 'room',
      targetNodeRid: rid,
      targetNodeGeneration: 1n,
      targetMeshName: 'mesh',
      descriptorVersion: '1'
    };
    const authority = new ZLinkInstanceActivationAuthority({
      store,
      relocationStore,
      meshName: 'mesh',
      owner: () => owners[rid],
    });
    runtime.registerAsyncInstanceActivationAuthority(authority);
    runtime.registerInstanceApplicationLifecycle({
      isMaterialized: () => false,
      async materialize() {
        factories++;
      },
      async discard() {},
      beginTerminal() {},
      async completeTerminal() {
        return false;
      }
    });
    const deadlineUnixMs = BigInt(Date.now() + 5000);
    const record = {
      kind: 'instanceSpot',
      activation: 'missing',
      target,
      sourceNodeRid: 'source',
      sourceNodeGeneration: 1n,
      operationKind: 'send',
      operation: { high: 1n, low: rid === 'node-a' ? 1n : 2n },
      deadlineUnixMs
    };
    const payload = encodeApplicationPayload({
      packetName: 'Notice',
      contentType: 'application/json',
      payload: Buffer.from('{}')
    });
    return Object.assign(() => runtime.runMissingInstanceActivation(record, payload), {
      runtime,
      authority,
      record,
      payload
    });
  }
  const resolver = new internal.ZLinkAuthoritySpotRouteResolver(store, (name) => name, undefined, {
    ownerTokenLeaseDeadlineMs: async () => -Infinity
  });
  let routed = 0;
  const selected = [];
  const transport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => resolver,
    routed: {
      async sendToSpot() {
        routed++;
        assert.fail('expired Ready must not route');
      }
    },
    meshNames: () => ['mesh', 'other'],
    meshNode: (name) => ({
      instanceSpotPlacementTypes: () => placementTypes,
      selectObjectPlacement(type) {
        selected.push({ name, type });
        return {
          kind: 'selected',
          target: { targetNodeRid: 'node-a', targetNodeGeneration: 1n, descriptorVersion: '1' }
        };
      },
      async requestToMissingInstanceSpot(target, encoded) {
        assert.equal(target.targetMeshName, 'mesh');
        assert.equal(target.stableType, 'room');
        await activate();
        return {
          terminalResult: 0,
          failureErrno: 0,
          parts: channelEnvelope
            .encodeChannelReplyParts(
              channelEnvelope.decodeChannelEnvelope(encoded.map(ZLinkBufferMessage.from)).header,
              { generation: 'new' }
            )
            .map(ZLinkBufferMessage.from)
        };
      },
      async sendToMissingInstanceSpot(target) {
        assert.equal(target.targetMeshName, 'mesh');
        assert.equal(target.stableType, 'room');
        await activate();
        return 0;
      }
    }),
    completions: () => ({
      submit(operation) {
        return operation();
      }
    }),
    defaultRequestTimeoutMs: 5000
  });
  const activate = activation('node-a');
  return {
    store,
    key,
    before: committed.ready,
    clock,
    owners,
    expireActorOwner() {
      now = new Date(62001);
    },
    removeFactoryTargetDuringRelease() {
      changeDuringRelease = () =>
        store.removeMeshNode({ meshName: 'mesh', rid: 'node-b' }, owners['node-b']);
    },
    get releaseWrite() {
      return releaseWrite;
    },
    activation,
    activate,
    resolver,
    transport,
    selected,
    roots,
    get factories() {
      return factories;
    },
    get routed() {
      return routed;
    },
    close() {
      for (const runtime of runtimes) runtime.close();
    }
  };
}

for (const provider of [false, true]) {
  const label = provider ? 'provider' : 'in-memory';
  test(`${label}: ended Ready release rejects a changed factory target descriptor`, async () => {
    const f = await fixture(provider);
    try {
      f.removeFactoryTargetDuringRelease();
      const release = f.store.releaseEndedReservation(
        f.key,
        f.before.storeVersion.value,
        undefined,
        {
          key: { kind: 'instance_spot', globalId: 'ended-room' },
          intent: { stableType: 'room' },
          target: { meshName: 'mesh', nodeRid: 'node-b', nodeLifecycleGeneration: 1n }
        }
      );
      if (provider) {
        await assert.rejects(release, { kind: framework.ZLinkFrameworkErrorKind.Unavailable });
        assert.equal(f.releaseWrite.kind, 'conflict');
      } else {
        assert.equal(await release, false);
      }
      const current = await f.store.readAuthority(f.key);
      assert.equal(current.kind, 'snapshot');
      assert.deepEqual(current.storeVersion, f.before.storeVersion);
      assert.equal((await f.store.listMeshNodes('mesh')).items.length, 1);
      assert.equal((await f.store.listMeshNodes('mesh')).items[0].populationCapacity.spots.active, 1);
    } finally {
      f.close();
    }
  });

  test(`${label}: Actor GetOrCreate uses the remote factory target after owner expiry`, async () => {
    const f = await fixture(provider);
    try {
      const { ZLinkActorPlacementCoordinator } = require(
        '../../packages/framework/dist/runtime/host/actor-placement-coordinator'
      );
      const { ZLinkRuntimeTaskErrorSink, ZLinkRuntimeTaskRunner } = require(
        '../../packages/framework/dist/runtime/execution'
      );
      const target = (nodeRid, sourceNodeRid) => ({
        meshName: 'mesh',
        nodeRid,
        nodeGeneration: 1n,
        entrySpotId: nodeRid + '-entry',
        owner: f.owners[nodeRid],
        isLocal: nodeRid === sourceNodeRid,
        sourceNodeRid,
        sourceNodeGeneration: 1n
      });
      const server = new ZLinkActorPlacementCoordinator({ store: f.store });
      const calls = [];
      const remoteCreate = (_mesh, rid, request) => server.handleRemoteCreate(
        { ...request, correlation: 1n },
        async (_payload, authority) => {
          calls.push(rid);
          return {
            result: 'created',
            actor: { actorId: 'player-1', nodeRid: rid, objectGeneration: authority.objectGeneration },
            entrySpotId: rid + '-entry',
            entrySpotGeneration: 1n
          };
        },
        new AbortController().signal
      );
      const leaseTracker = {
        async isOwnerTokenLive(owner) {
          const lease = await f.store.readOwnerLease(owner.ownerId);
          return lease.kind === 'found' && lease.token.leaseGeneration === owner.leaseGeneration &&
            lease.leaseExpiresAt > lease.storeNow;
        }
      };
      const old = new ZLinkActorPlacementCoordinator({
        store: f.store,
        leaseTracker,
        target: async () => target('node-a', 'node-a'),
        remoteCreate
      });
      const before = await old.create('player-1', 'player', false, 'mesh', Buffer.alloc(0), 1000);
      f.expireActorOwner();
      let selections = 0;
      const client = new ZLinkActorPlacementCoordinator({
        store: f.store,
        leaseTracker,
        target: async () => {
          selections++;
          return target('node-b', 'client');
        },
        remoteCreate
      });
      const manager = new internal.DefaultZLinkActorManager({
        placementCreate: (id, type, createOnly, options, signal) =>
          client.create(id, type, createOnly, options.meshName, Buffer.alloc(0), 1000, signal)
      }, new ZLinkRuntimeTaskRunner(new ZLinkRuntimeTaskErrorSink(), new AbortController().signal));
      const getOrCreate = () => manager.getOrCreate('player-1', 'player').inMesh('mesh').submit();
      await assert.rejects(getOrCreate, { kind: framework.ZLinkFrameworkErrorKind.Unavailable });
      f.clock.monotonicMs += 3;
      const after = await getOrCreate();
      assert.equal(after.status, 'created');
      assert.equal(after.actor.nodeRid, 'node-b');
      assert.ok(after.actor.objectGeneration > before.actor.objectGeneration);
      assert.deepEqual(calls, ['node-a', 'node-b']);
      assert.equal(selections, 2);
    } finally {
      f.close();
    }
  });

  test(`${label}: expired steady Ready Instance intent recreates once using stored type and Mesh`, async () => {
    const f = await fixture(provider);
    try {
      class Notice {}
      const result = await f.transport.sendToSpotAddress('ended-room', new Notice(), {
        instanceSpot: true
      });
      assert.equal(result.status, ZLinkSubmitStatus.Submitted);
      const after = await f.store.readAuthority(f.key);
      assert.ok(after.objectGeneration > f.before.objectGeneration);
      assert.equal(f.factories, 1);
      assert.equal(f.routed, 0);
      assert.deepEqual(f.selected, [{ name: 'mesh', type: 'room' }]);
      // The same node's capacity limit is one: Reserve and Commit can only
      // succeed if the old active allocation was returned by the release batch.
      assert.equal(after.allocation.state, 'active');
    } finally {
      f.close();
    }
  });
  test(`${label}: cold activation joins the same target's live Ready Spot`, async () => {
    const f = await fixture(provider);
    try {
      const first = await f.activate();
      const before = await f.store.readAuthority(f.key);
      const second = await f.activate();
      assert.equal(second.spot, first.spot);
      assert.deepEqual(second.route, first.route);
      assert.equal(f.factories, 1);
      assert.deepEqual(await f.store.readAuthority(f.key), before);
    } finally {
      f.close();
    }
  });
  for (const pending of [false, true])
    test(`${label}: Ready local admission survives recovery deletion after authority snapshot (pending=${pending})`, async () => {
      const f = await fixture(provider);
      const { runtime, record } = f.activate;
      const request = { ...record, operationKind: 'request', replyRouteId: 1n };
      const frame = (value) =>
        encodeApplicationPayload({
          packetName: 'Echo',
          contentType: 'application/json',
          payload: Buffer.from(JSON.stringify({ value }))
        });
      const firstPayload = frame('first');
      const secondPayload = frame('second');
      const echo = (payload) => JSON.parse(decodeApplicationPayload(payload).payload.toString());
      let releaseHandler;
      const heldHandler = new Promise((resolve) => {
        releaseHandler = resolve;
      });
      let returnRead;
      const heldRead = new Promise((resolve) => {
        returnRead = resolve;
      });
      let snapshotRead;
      const captured = new Promise((resolve) => {
        snapshotRead = resolve;
      });
      let firstResponse;
      try {
        await runtime.activateMissingInstanceAsync(
          request,
          firstPayload,
          undefined,
          ({ route }) => {
            firstResponse = heldHandler.then(async () => {
              await runtime.completeRecoveredInstanceActivation(record.target, route);
              return echo(firstPayload);
            });
          }
        );
        if (!pending) await new Promise((resolve) => setImmediate(resolve));
        const originalRead = f.store.readAuthority.bind(f.store);
        let hold = true;
        f.store.readAuthority = async (key) => {
          const snapshot = await originalRead(key);
          if (hold && key.value === f.key.value) {
            hold = false;
            snapshotRead(snapshot);
            await heldRead;
          }
          return snapshot;
        };
        let secondResponse;
        const second = runtime.activateMissingInstanceAsync(
          { ...request, operation: { high: 1575n, low: 2n }, replyRouteId: 2n },
          secondPayload,
          undefined,
          () => {
            secondResponse = echo(secondPayload);
          }
        );
        // Attach failure observation before releasing the first handler.
        const result = second.then(
          () => ({ value: secondResponse }),
          (error) => ({ error })
        );
        const snapshot = await captured;
        const authority = internal.decodeServiceInstanceAuthorityPayload(snapshot.payload);
        const reference = authority.activationRecovery.reference;
        assert.equal(f.roots.has(reference), true);
        releaseHandler();
        assert.deepEqual(await firstResponse, { value: 'first' });
        assert.equal(f.roots.has(reference), false);
        returnRead();
        const outcome = await result;
        if (outcome.error !== undefined) throw outcome.error;
        assert.deepEqual(outcome.value, { value: 'second' });
        assert.equal(f.factories, 1);
      } finally {
        releaseHandler();
        returnRead();
        f.close();
      }
    });
  test(`${label}: a declined release uses a fresh authority read`, async () => {
    const f = await fixture(provider);
    try {
      const snapshot = await f.store.readAuthority(f.key);
      let reads = 0;
      const authority = new ZLinkInstanceActivationAuthority({
        store: {
          async readAuthority() {
            return ++reads === 1 ? snapshot : { kind: 'missing' };
          },
          async releaseEndedReservation() {
            return false;
          }
        },
        meshName: 'mesh',
        owner: () => undefined,
      });
      assert.deepEqual(
        await authority.read(
          {
            targetSpotId: 'ended-room',
            stableType: 'room',
            targetNodeRid: 'node-a',
            targetNodeGeneration: 1n
          },
          true
        ),
        { kind: 'missing' }
      );
      assert.equal(reads, 2);
      assert.equal(f.factories, 0);
    } finally {
      f.close();
    }
  });
  test(`${label}: expired Ready request uses cold activation under its original deadline`, async () => {
    const f = await fixture(provider);
    try {
      class Lookup {}
      const reply = await f.transport.requestToSpotAddress('ended-room', new Lookup(), {
        instanceSpot: true
      });
      assert.equal(reply.generation, 'new');
      assert.equal(f.factories, 1);
      assert.ok((await f.store.readAuthority(f.key)).objectGeneration > f.before.objectGeneration);
    } finally {
      f.close();
    }
  });
  test(`${label}: source placement registration does not override target policy`, async () => {
    const f = await fixture(provider, 'recreate');
    try {
      const before = await f.store.readAuthority(f.key);
      class Notice {}
      await assert.rejects(
        f.transport.sendToSpotAddress('ended-room', new Notice(), { instanceSpot: true }),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      assert.deepEqual(f.selected, [{ name: 'mesh', type: 'room' }]);
      assert.equal(f.factories, 0);
      assert.deepEqual(await f.store.readAuthority(f.key), before);
    } finally {
      f.close();
    }
  });
  test(`${label}: expired Ready without an eligible source target stays Unavailable`, async () => {
    const f = await fixture(provider, 'disabled', []);
    try {
      const before = await f.store.readAuthority(f.key);
      class Notice {}
      assert.equal(
        (await f.transport.sendToSpotAddress('ended-room', new Notice(), { instanceSpot: true }))
          .status,
        ZLinkSubmitStatus.RouteNotConnected
      );
      await assert.rejects(
        f.transport.requestToSpotAddress('ended-room', new Notice(), { instanceSpot: true }),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      assert.equal(f.factories, 0);
      assert.deepEqual(await f.store.readAuthority(f.key), before);
    } finally {
      f.close();
    }
  });
  test(`${label}: ordinary message and Find do not release an expired Ready`, async () => {
    const f = await fixture(provider);
    try {
      await assert.rejects(
        f.transport.sendToSpotAddress('ended-room', {}, {}),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      await assert.rejects(
        f.resolver.resolve('ended-room'),
        (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
      );
      assert.deepEqual(await f.store.readAuthority(f.key), {
        ...f.before,
        storeNow: new Date(32000)
      });
      assert.equal(f.factories, 0);
      assert.equal(f.selected.length, 0);
    } finally {
      f.close();
    }
  });
  for (const policy of ['recreate', 'snapshot', null]) {
    test(`${label}: target policy ${policy} refuses without factory or Store mutation`, async () => {
      const f = await fixture(provider, policy);
      try {
        const before = await f.store.readAuthority(f.key);
        await assert.rejects(
          f.activate(),
          (error) => error.kind === framework.ZLinkFrameworkErrorKind.Unavailable
        );
        assert.deepEqual(await f.store.readAuthority(f.key), before);
        assert.equal(f.factories, 0);
      } finally {
        f.close();
      }
    });
  }
  test(`${label}: explicit type mismatch preserves expired authority`, async () => {
    const f = await fixture(provider);
    try {
      const before = await f.store.readAuthority(f.key);
      await assert.rejects(
        f.transport.sendToSpotAddress(
          'ended-room',
          {},
          { instanceSpot: true, instanceSpotType: 'other-room' }
        ),
        (error) =>
          internalFrameworkErrorKind(error) === ZLinkFrameworkInternalErrorKind.SpotTypeMismatch
      );
      assert.deepEqual(await f.store.readAuthority(f.key), before);
      assert.equal(f.factories, 0);
      assert.equal(f.selected.length, 0);
    } finally {
      f.close();
    }
  });
  test(`${label}: two targets compete for one successor; loser is Unavailable`, async () => {
    const f = await fixture(provider);
    try {
      const results = await Promise.allSettled([f.activate(), f.activation('node-b')()]);
      assert.equal(results.filter((result) => result.status === 'fulfilled').length, 1);
      const loser = results.find((result) => result.status === 'rejected');
      assert.equal(loser.reason.kind, framework.ZLinkFrameworkErrorKind.Unavailable);
      assert.equal(f.factories, 1);
      const after = await f.store.readAuthority(f.key);
      assert.ok(after.objectGeneration > f.before.objectGeneration);
    } finally {
      f.close();
    }
  });
}
