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
  ZLinkLocationWriteIntent
} = require('../../packages/framework/dist/contracts/Locations/Writes');
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
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const channelEnvelope = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const {
  encodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

async function fixture(
  provider,
  targetPolicy = 'disabled',
  placementTypes = ['room', 'other-room']
) {
  let now = new Date(1000);
  const store = provider
    ? new ZLinkLocationStoreRepository(new ZLinkInMemoryProviderLocationStore(() => now), () => now)
    : new internal.ZLinkInMemoryLocationStore(() => now);
  async function descriptor(ownerId, rid) {
    const lease = await store.claimOwnerLease(ownerId, 30000);
    assert.equal(lease.kind, 'claimed');
    const owner = lease.token;
    await store.updateMeshNode(
      {
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
            objectKind: 'instance_spot',
            stableType: 'room',
            policy: 'disabled',
            hasSnapshotAdapter: false,
            limit: 1
          }
        ],
        state: framework.ZLinkFrameworkRuntimeState.Serving,
        securityIdentity: 'test',
        ownerId,
        leaseGeneration: owner.leaseGeneration,
        updatedAt: now
      },
      ZLinkLocationWriteIntent.NewClaim
    );
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
    actorRelocationPolicy: 'disabled',
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
    'node-a': await descriptor('new-owner-a', 'node-a'),
    'node-b': await descriptor('new-owner-b', 'node-b')
  };
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
  function activation(rid, policy = 'disabled') {
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
    runtime.registerAsyncInstanceActivationAuthority(
      new ZLinkInstanceActivationAuthority({
        store,
        relocationStore,
        meshName: 'mesh',
        owner: () => owners[rid],
        relocationPolicy: () => policy ?? undefined
      })
    );
    runtime.registerInstanceApplicationLifecycle({
      isMaterialized: () => false,
      async materialize() {
        factories++;
      },
      async discard() {}
    });
    return () =>
      runtime.runMissingInstanceActivation(
        {
          kind: 'instanceSpot',
          activation: 'missing',
          target,
          sourceNodeRid: 'source',
          sourceNodeGeneration: 1n,
          operationKind: 'send',
          operation: { high: 1n, low: rid === 'node-a' ? 1n : 2n },
          deadlineUnixMs: BigInt(Date.now() + 5000)
        },
        encodeApplicationPayload({
          packetName: 'Notice',
          contentType: 'application/json',
          payload: Buffer.from('{}')
        })
      );
  }
  const resolver = new internal.ZLinkAuthoritySpotRouteResolver(store, (name) => name, undefined, {
    remainingOwnerTokenLeaseMs: async () => 0
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
  const activate = activation('node-a', targetPolicy);
  return {
    store,
    key,
    before: committed.ready,
    activation,
    activate,
    resolver,
    transport,
    selected,
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
      const f = await fixture(provider);
      try {
        const before = await f.store.readAuthority(f.key);
        await assert.rejects(
          f.activation('node-a', policy)(),
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
