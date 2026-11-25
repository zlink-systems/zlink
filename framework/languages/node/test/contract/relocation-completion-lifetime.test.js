const assert = require('node:assert/strict');
const test = require('node:test');
const { completionFixture, example } = require('./helpers/relocation-completion');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} = require('../../packages/framework/dist/contracts/Errors/ZLinkFrameworkException');
const {
  encodeAuthorityKey
} = require('../../packages/framework/dist/runtime/locations/authority-key-codec');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  encodeMaintenanceReplyRelayAck,
  decodeMaintenanceReplyRelay
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  decodeRelocationManifestV1
} = require('../../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated');

const actorKey = encodeAuthorityKey('actor', 'completion-test');
const hour = 60 * 60 * 1000;
const source = { ownerId: 'source', leaseGeneration: 1n, nodeRid: 'source', nodeGeneration: 2n };
const coordinator = {
  ownerId: 'target',
  leaseGeneration: 1n,
  nodeRid: 'target',
  nodeGeneration: 3n,
  expectedAuthorityStoreVersion: '1'
};
const packet = {
  index: 0,
  returnResponse: true,
  source: {
    ownerId: source.ownerId,
    ownerLeaseGeneration: String(source.leaseGeneration),
    nodeRid: source.nodeRid,
    nodeGeneration: String(source.nodeGeneration),
    replyRouteId: '1'
  },
  messageFollowContext: { operationId: '00000000000000000000000000000001' }
};

function runtimeAtDelivery(f, observeActorAuthorityVersion) {
  const stage = {
    lane: Promise.resolve(),
    offer: { prepare: { targetAttemptGeneration: 1n } },
    staging: {
      primaryAuthorityKey: f.key,
      envelope: {
        aggregateId: example.relocationId,
        participants: [{ key: f.key.value, objectKind: 'actor' }]
      }
    }
  };
  async function ack(request) {
    const bytes = encodeMaintenanceReplyRelayAck({
      ...request,
      requestSource: source,
      status: 'terminalReceived'
    });
    await runtime.tryHandleControl('mesh', {
      sourceNodeRid: source.nodeRid,
      parts: [{ data: () => bytes, close() {} }]
    });
  }
  f.authorities.readAuthority = async () => f.current();
  f.authorities.readOwnerLease = async (ownerId) => ({
    kind: 'found',
    token: { ownerId, leaseGeneration: source.leaseGeneration },
    storeNow: new Date(0),
    leaseExpiresAt: new Date(hour)
  });
  const node = {
    sendInfrastructureControl() {
      throw new Error('expected multipart reply');
    },
    async sendInfrastructureControlFrames(_rid, frames) {
      await ack(decodeMaintenanceReplyRelay(frames));
      return SubmitResult.Ok;
    }
  };
  const runtime = new ZLinkHostServiceRelocationRuntime({
    registration: { messageSerializers: [] },
    relocationStore: f.store,
    observeActorAuthorityVersion,
    locationStore: () => f.authorities,
    currentOwner: () => ({
      ownerId: coordinator.ownerId,
      leaseGeneration: coordinator.leaseGeneration
    }),
    meshNode: () => node,
    liveDescriptors: async () => [
      {
        rid: source.nodeRid,
        ownerId: source.ownerId,
        leaseGeneration: source.leaseGeneration,
        lifecycleGeneration: source.nodeGeneration,
        state: 1
      }
    ]
  });
  return {
    runtime,
    stage,
    ack,
    node,
    deliver: () =>
      runtime.relayTerminalReply('mesh', stage, coordinator, 1n, packet, {
        index: 0,
        ok: true,
        response: { answer: 1 }
      })
  };
}

test('terminal cleanup unpublishes before deleting the ACKed manifest and chunks', async () => {
  for (const loseClearResponse of [false, true]) {
    const f = completionFixture(1n, actorKey);
    const target = runtimeAtDelivery(f);
    const terminal = await target.deliver();
    assert.equal(terminal.relocation.completion.pendingDeliveries, 0n);
    const root = terminal.relocation.completion.reference;
    const manifest = decodeRelocationManifestV1(f.blobs.get(root));
    const superseded = [...f.blobs.keys()].find(
      (ref) => ref !== root && ref !== manifest.chunks[0].reference
    );
    if (loseClearResponse) {
      const compareExchange = f.authorities.compareExchangeAuthority;
      f.authorities.compareExchangeAuthority = async (...args) => {
        await compareExchange(...args);
        throw new Error('injected lost unpublish CAS response');
      };
    }
    const start = f.events.length;
    await target.runtime.clearTargetRelocationPublication(target.stage, terminal);
    assert.equal(f.current().relocation, null);
    assert.deepEqual(
      f.events.slice(start).filter((item) => item !== 'read'),
      ['cas', `delete:${manifest.chunks[0].reference}`, `delete:${root}`]
    );
    assert.equal(f.blobs.has(root), false);
    assert.equal(f.blobs.has(superseded), true);
  }
});

test('a rejected terminal clear preserves the published manifest and chunks', async () => {
  const f = completionFixture(1n, actorKey);
  const target = runtimeAtDelivery(f);
  const terminal = await target.deliver();
  const before = new Map(f.blobs);
  f.authorities.compareExchangeAuthority = async () => ({ kind: 'conflict' });
  await assert.rejects(target.runtime.clearTargetRelocationPublication(target.stage, terminal));
  assert.deepEqual(f.current(), terminal);
  assert.deepEqual(f.blobs, before);
});

test('terminal unpublish invalidates the actor route cache even when blob deletion fails', async () => {
  const f = completionFixture(1n, actorKey);
  const versions = [];
  const target = runtimeAtDelivery(f, (_actorId, version) => versions.push(version));
  const terminal = await target.deliver();
  f.store.delete = async () => {
    throw new Error('injected blob delete outage');
  };
  await assert.rejects(
    target.runtime.clearTargetRelocationPublication(target.stage, terminal),
    /injected blob delete outage/
  );
  assert.equal(f.current().relocation, null);
  assert.deepEqual(versions, [f.current().storeVersion.value]);
});

test(
  'pending completion stays readable through a lost reply and Location Store outage',
  { timeout: 5000 },
  async (t) => {
    const f = completionFixture(1n, actorKey);
    const target = runtimeAtDelivery(f);
    t.after(() => target.runtime.dispose());
    const readLease = f.authorities.readOwnerLease;
    const send = target.node.sendInfrastructureControlFrames;
    let losingStoreReads = false;
    let authorityFailures = 0;
    let sends = 0;
    target.node.sendInfrastructureControlFrames = async (...args) => {
      if (++sends === 1) {
        // The next retry cannot check either the authority or source lease.
        losingStoreReads = true;
        f.advance(12 * hour);
        return SubmitResult.Ok;
      }
      return send(...args);
    };
    f.authorities.readAuthority = async () => {
      if (losingStoreReads && authorityFailures < 2) {
        f.advance(++authorityFailures === 1 ? 11 * hour : 2 * hour);
        throw new Error('injected authority outage');
      }
      return f.current();
    };
    f.authorities.readOwnerLease = async (ownerId) => {
      if (losingStoreReads && authorityFailures === 0) {
        losingStoreReads = false;
        f.advance(13 * hour);
        throw new Error('injected lease outage');
      }
      return readLease(ownerId);
    };
    const terminal = await target.deliver();
    assert.equal(terminal.relocation.completion.pendingDeliveries, 0n);
    assert.equal(sends, 2);
    assert.equal(authorityFailures, 2);
    assert.ok(f.events.some((item) => item.startsWith('renew:')));
  }
);

test(
  'a received ACK keeps its pending blob during Location Store recovery',
  { timeout: 5000 },
  async (t) => {
    const f = completionFixture(1n, actorKey);
    const target = runtimeAtDelivery(f);
    t.after(() => target.runtime.dispose());
    const send = target.node.sendInfrastructureControlFrames;
    let unavailable = false;
    let failures = 0;
    target.node.sendInfrastructureControlFrames = async (...args) => {
      unavailable = true;
      return send(...args);
    };
    f.authorities.readAuthority = async () => {
      if (unavailable && failures < 2) {
        f.advance(++failures === 1 ? 12 * hour : 13 * hour);
        throw new Error('injected authority outage after ACK');
      }
      return f.current();
    };
    const terminal = await target.deliver();
    assert.equal(failures, 2);
    assert.equal(terminal.relocation.completion.pendingDeliveries, 0n);
    assert.ok(f.events.some((item) => item.startsWith('renew:')));
  }
);

test(
  'a completion published by a CAS with a lost response survives a Location Store outage',
  { timeout: 5000 },
  async (t) => {
    const f = completionFixture(1n, actorKey);
    const target = runtimeAtDelivery(f);
    t.after(() => target.runtime.dispose());
    const compareExchange = f.authorities.compareExchangeAuthority;
    let responseLost = false;
    let failures = 0;
    f.authorities.compareExchangeAuthority = async (...args) => {
      const result = await compareExchange(...args);
      if (!responseLost) {
        responseLost = true;
        throw new Error('injected lost completion CAS response');
      }
      return result;
    };
    f.authorities.readAuthority = async () => {
      if (responseLost && failures < 2) {
        f.advance(++failures === 1 ? 12 * hour : 11 * hour);
        throw new Error('injected authority outage after completion CAS');
      }
      if (failures === 2) {
        failures++;
        f.advance(2 * hour);
      }
      return f.current();
    };
    const terminal = await target.deliver();
    assert.equal(failures, 3);
    assert.equal(terminal.relocation.completion.pendingDeliveries, 0n);
    assert.ok(f.events.some((item) => item.startsWith('renew:')));
  }
);

test('a corrupt authority readback terminates the completion CAS', { timeout: 5000 }, async (t) => {
  const f = completionFixture(1n, actorKey);
  const target = runtimeAtDelivery(f);
  t.after(() => target.runtime.dispose());
  const dataLost = new ZLinkFrameworkException(
    ZLinkFrameworkErrorKind.DataLost,
    'corrupt authority'
  );
  let submitted = false;
  let failedReads = 0;
  f.authorities.compareExchangeAuthority = async () => {
    submitted = true;
    throw new Error('injected lost completion CAS response');
  };
  f.authorities.readAuthority = async () => {
    if (submitted) {
      failedReads++;
      if (failedReads === 1) throw dataLost;
      throw new TypeError('unexpected second read');
    }
    return f.current();
  };
  await assert.rejects(target.deliver(), (error) => error === dataLost);
  assert.equal(failedReads, 1);
});
