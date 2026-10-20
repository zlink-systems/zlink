'use strict';

// Cross-language source request owner contract (fixture
// framework/runtime/protocol/fixtures/source-request-owner.json): the first accepting owner reads
// OwnerId and LeaseGeneration from the admitted peer's descriptor, from the polled descriptors
// first and the Store once otherwise, and ends the request Unavailable without a live descriptor.

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');
const {
  decodeRoutingId,
  encodeRoutingIdStorageHex
} = require('../../packages/framework/dist/runtime/routing-id');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist');
const {
  internalFrameworkWireReply
} = require('../../packages/framework/dist/runtime/framework-errors-internal');

const LEASE_TTL_MS = 1000;
const schema = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/protocol/service-wire-v1.schema.json'),
    'utf8'
  )
);
const fixture = JSON.parse(
  fs.readFileSync(
    path.resolve(__dirname, '../../../../runtime/protocol/fixtures/source-request-owner.json'),
    'utf8'
  )
);

for (const scenario of fixture.cases) {
  test(`SourceRequestOwner ${scenario.name}`, async () => {
    const meshName = fixture.meshName;
    const nodeRid = fixture.peer.nodeRid;
    const nodeRidStorageHex = encodeRoutingIdStorageHex(nodeRid);
    const nodeGeneration = BigInt(fixture.peer.nodeGeneration);
    const ownerId = fixture.descriptor.ownerId;
    const leaseGeneration = BigInt(fixture.descriptor.leaseGeneration);
    const descriptor = {
      meshName,
      rid: nodeRid,
      lifecycleGeneration: nodeGeneration,
      ownerId,
      leaseGeneration
    };
    let nowMs = 1;
    let fetchedAtMs = nowMs;
    let descriptorReads = 0;
    const resolver = new ZLinkRequestSourceResolver({
      monotonicNowMs: () => nowMs,
      listLiveMeshNodes: async () => {
        descriptorReads += 1;
        fetchedAtMs = nowMs;
        return scenario.stored ? [descriptor] : [];
      },
      remainingOwnerTokenLeaseMs: async (owner) =>
        owner.ownerId === ownerId && owner.leaseGeneration === leaseGeneration
          ? Math.max(0, LEASE_TTL_MS - (nowMs - fetchedAtMs))
          : 0
    });
    if (scenario.polled) {
      // The auto-connect poll observes the live descriptors.
      await resolver.observeLiveDescriptors(meshName, [descriptor]);
    }
    if (!scenario.leaseLive) nowMs += LEASE_TTL_MS * 2;

    const recorded = await resolver.accept(meshName, nodeRid, nodeGeneration, nodeRidStorageHex);

    assert.equal(descriptorReads, scenario.storeDescriptorReads);
    if (scenario.result === 'accepted') {
      const expected = { ownerId, leaseGeneration, nodeRid, nodeGeneration };
      assert.deepEqual(recorded, expected);
      // The Store answer joins the polled descriptors; the next request does not read the Store.
      assert.deepEqual(
        resolver.accept(meshName, nodeRid, nodeGeneration, nodeRidStorageHex),
        expected
      );
      assert.equal(descriptorReads, scenario.storeDescriptorReads);
    } else {
      assert.equal(recorded, undefined);
      // The accepting owner ends the request with the fixture's public result and wire code.
      assert.equal(
        ZLinkFrameworkErrorKind[fixture.unavailable.errorKind],
        ZLinkFrameworkErrorKind.Unavailable
      );
      assert.deepEqual(
        internalFrameworkWireReply(
          new ZLinkFrameworkException(ZLinkFrameworkErrorKind.Unavailable, 'source')
        ),
        {
          terminalResult: wireValue(fixture.unavailable.terminalResult),
          failureCode: wireValue(fixture.unavailable.failureCode)
        }
      );
    }
  });
}

function wireValue(name) {
  const found = [];
  (function walk(node) {
    if (Array.isArray(node)) node.forEach(walk);
    else if (node !== null && typeof node === 'object') {
      if (node.name === name && typeof node.value === 'number') found.push(node.value);
      Object.values(node).forEach(walk);
    }
  })(schema);
  assert.equal(new Set(found).size, 1, `wire name ${name}`);
  return found[0];
}

for (const polled of [false, true]) {
  test(`SourceRequestOwner compares opaque RID bytes polled=${polled}`, async () => {
    const descriptor = {
      rid: decodeRoutingId('same display', 'ff00'),
      lifecycleGeneration: 5n,
      ownerId: 'owner',
      leaseGeneration: 3n
    };
    let reads = 0;
    const resolver = new ZLinkRequestSourceResolver({
      listLiveMeshNodes: async () => {
        reads++;
        return [descriptor];
      },
      remainingOwnerTokenLeaseMs: async () => 1000
    });
    if (polled) await resolver.observeLiveDescriptors('mesh', [descriptor]);
    const source = await resolver.accept(
      'mesh',
      decodeRoutingId('other display', 'ff00'),
      5n,
      'ff00'
    );
    assert.equal(source?.ownerId, 'owner');
    assert.equal(reads, polled ? 0 : 1);
    assert.strictEqual(resolver.accept('mesh', decodeRoutingId('', 'ff00'), 5n, 'ff00'), source);
    assert.equal(reads, polled ? 0 : 1);
    assert.equal(
      await resolver.accept('mesh', decodeRoutingId('same display', 'ff01'), 5n, 'ff01'),
      undefined
    );
    assert.equal(await resolver.accept('mesh', decodeRoutingId('', 'ff00'), 6n, 'ff00'), undefined);
  });
}

test('SourceRequestOwner cache hit reuses the received RID storage key', async () => {
  const storageKey = encodeRoutingIdStorageHex('sender');
  const resolver = new ZLinkRequestSourceResolver({
    listLiveMeshNodes: async () => [],
    remainingOwnerTokenLeaseMs: async () => 1000
  });
  await resolver.observeLiveDescriptors('mesh', [
    {
      rid: 'sender',
      lifecycleGeneration: 5n,
      ownerId: 'owner',
      leaseGeneration: 3n
    }
  ]);

  const originalToString = Buffer.prototype.toString;
  let hexStrings = 0;
  Buffer.prototype.toString = function (encoding, ...args) {
    if (encoding === 'hex') hexStrings += 1;
    return Reflect.apply(originalToString, this, [encoding, ...args]);
  };
  try {
    const source = resolver.accept('mesh', 'sender', 5n, storageKey);
    assert.equal(source?.ownerId, 'owner');
    assert.equal(hexStrings, 0, 'cache hits reuse the storage key from ingress');
  } finally {
    Buffer.prototype.toString = originalToString;
  }
});

test('SourceRequestOwner uses received RID bytes instead of its display string', async () => {
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) }
    },
    'local',
    7n
  );
  runtime.setRequestSourceAcceptance((rid, generation, storageKey) => {
    assert.equal(storageKey, 'ff00');
    assert.equal(
      require('../../packages/framework/dist/runtime/routing-id').encodeRoutingIdStorageHex(rid),
      'ff00'
    );
    return { ownerId: 'owner', leaseGeneration: 3n, nodeRid: rid, nodeGeneration: generation };
  });
  const ingress = { sourceRoutingId: 'display', sourceRoute: Buffer.from('ff00', 'hex') };
  await runtime.acceptRequestSource(ingress);
  assert.equal(ingress.requestSource.ownerId, 'owner');
});

test('SourceRequestOwner propagates programming errors from Store reads', async () => {
  const error = new TypeError('broken descriptor reader');
  const resolver = new ZLinkRequestSourceResolver({
    listLiveMeshNodes: async () => {
      throw error;
    },
    remainingOwnerTokenLeaseMs: async () => 1000
  });
  await assert.rejects(
    resolver.accept('mesh', 'sender', 1n, encodeRoutingIdStorageHex('sender')),
    (actual) => actual === error
  );
});

const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ReceiveKind
} = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const {
  encodeActorHeader,
  M6bServiceWireCommand
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  encodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

test('Spot and Actor source preparation adds no per-record dispatch closures', () => {
  const records = [];
  const runtime = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      mailbox: {
        tryEnqueue(record) {
          records.push(record);
          return true;
        }
      }
    },
    'local',
    7n
  );
  runtime.setRequestSourceAcceptance(() => undefined);

  for (const receiveKind of [ReceiveKind.SpotSend, ReceiveKind.ActorSend]) {
    runtime.enqueueApplicationFrame(
      {
        sourceRoutingId: 'sender',
        parts: [Buffer.from('header')],
        applicationJobOwner: { takeInitial: () => ({ close() {} }) }
      },
      'owner',
      Buffer.from('payload'),
      { receiveKind }
    );
  }

  assert.equal(records.length, 2);
  assert.equal(records[0].prepareDispatch, undefined);
  assert.equal(records[1].prepareDispatch, undefined);
  assert.strictEqual(records[0].sourcePreparationOwner, runtime);
  assert.strictEqual(records[1].sourcePreparationOwner, runtime);
  assert.strictEqual(
    records[0].sourcePreparationOwner.prepareDispatch,
    records[1].sourcePreparationOwner.prepareDispatch
  );
});

test('SourceRequestOwner reports typed Store failure and retains its cause', async () => {
  const cause = new Error('connection closed');
  const error = new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.Unavailable,
    'Store offline',
    cause
  );
  const resolver = new ZLinkRequestSourceResolver({
    listLiveMeshNodes: async () => {
      throw error;
    },
    remainingOwnerTokenLeaseMs: async () => 1000
  });
  await assert.rejects(
    resolver.accept('mesh', 'sender', 5n, encodeRoutingIdStorageHex('sender')),
    (actual) => actual === error
  );
  const events = [];
  const replies = [];
  let queued;
  const raw = {
    setServiceIngress() {},
    topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) },
    mailbox: {
      tryEnqueue(record) {
        queued = record;
        return true;
      }
    },
    replyService: (_record, parts) => replies.push(parts)
  };
  const runtime = new ServiceStatefulRuntime(raw, 'local', 7n);
  runtime.createActor('actor');
  runtime.setDispatchErrorReporter(
    { captureEnabled: () => true, report: (event) => events.push(event) },
    'mesh'
  );
  runtime.setRequestSourceAcceptance((rid, generation, storageKey, signal) =>
    resolver.accept('mesh', rid, generation, storageKey, signal)
  );
  assert.equal(
    await runtime.ingress({
      command: M6bServiceWireCommand.actorRequest,
      flags: 0,
      sourceRoutingId: 'sender',
      requestSequence: 1n,
      applicationJobOwner: { takeInitial: () => ({ close() {} }) },
      parts: [
        encodeActorHeader(
          'actorRequest',
          {
            actor: { nodeRid: 'local', actorId: 'actor', generation: 1n },
            targetNodeGeneration: 7n,
            authorityOwnerGeneration: 1n,
            ownerLeaseGeneration: 1n
          },
          1n
        ),
        encodeApplicationPayload({
          packetName: 'Probe',
          contentType: 'application/json',
          payload: Buffer.from('{}')
        })
      ]
    }),
    'application'
  );
  assert.equal(events.length, 0, 'source lookup belongs to the claimed record');
  assert.equal(
    await queued.sourcePreparationOwner.prepareDispatch(queued, new AbortController().signal),
    false
  );
  assert.equal(replies.length, 1);
  assert.equal(events.length, 1);
  assert.strictEqual(events[0].error, error);
  assert.strictEqual(events[0].error.cause, cause);
});

for (const asynchronous of [false, true]) {
  for (const local of [false, true]) {
    test(`SourceRequestOwner records ingress once async=${asynchronous} local=${local}`, async () => {
      const raw = {
        setServiceIngress() {},
        topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) }
      };
      const runtime = new ServiceStatefulRuntime(raw, 'local', 7n);
      const source = {
        ownerId: 'owner',
        leaseGeneration: 3n,
        nodeRid: local ? 'local' : 'sender',
        nodeGeneration: local ? 7n : 5n
      };
      let reads = 0;
      runtime.setRequestSourceAcceptance((rid, generation) => {
        reads++;
        assert.equal(rid, source.nodeRid);
        assert.equal(generation, source.nodeGeneration);
        return asynchronous ? Promise.resolve(source) : source;
      });
      const ingress = { sourceRoutingId: source.nodeRid };
      await runtime.acceptRequestSource(ingress);
      assert.strictEqual(ingress.requestSource, source);
      await runtime.acceptRequestSource(ingress);
      assert.equal(reads, 1);
    });
  }
}
