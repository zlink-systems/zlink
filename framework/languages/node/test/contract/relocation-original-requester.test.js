const routingIds = require('../../packages/framework/dist/runtime/routing-id');
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ZLinkHostServiceRelocationRuntime
} = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const multipart = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  RequestResult,
  SubmitResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  OperationRegistry
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');

function fixture(targetRid = 'C', expired = false, sourceRid = 'A') {
  let submitted;
  const requester = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      requestService(_rid, parts) {
        submitted = parts;
        return new Promise(() => {});
      }
    },
    String(sourceRid),
    7n
  );
  const header = protocol.encodeStreamHeader({
    kind: protocol.ZLinkStreamMessageKind.Request,
    codec: protocol.ZLinkStreamCodec.Json,
    flags: protocol.ZLinkStreamHeaderFlags.None,
    requestSeq: 55n,
    name: 'Probe',
    metadata: new Map(),
    correlationId: 'original-A'
  });
  const pending = requester.requestToActor(
    { nodeRid: 'B', actorId: 'actor', generation: 1n },
    { packetName: 'Probe', contentType: 'application/json', payload: Buffer.from('{}') },
    1000,
    undefined,
    undefined,
    undefined,
    {
      actor: { nodeRid: 'B', actorId: 'actor', generation: 1n },
      targetNodeGeneration: 8n,
      authorityOwnerGeneration: 1n,
      ownerLeaseGeneration: 3n
    }
  );
  const operation = wire.decodeStatefulHeader(submitted[0]).operation;
  const source = {
    ownerId: 'owner-A',
    leaseGeneration: 9n,
    nodeRid: sourceRid,
    nodeGeneration: 7n
  };
  const coordinator = {
    ownerId: `owner-${targetRid}`,
    leaseGeneration: 4n,
    nodeRid: targetRid,
    nodeGeneration: 11n,
    expectedAuthorityStoreVersion: 'committed'
  };
  const descriptors = [source, coordinator].map((value) => ({
    ...value,
    rid: value.nodeRid,
    lifecycleGeneration: value.nodeGeneration,
    state: 1
  }));
  const leaseReads = [];
  const store = {
    async readOwnerLease(ownerId) {
      leaseReads.push(ownerId);
      assert.equal(ownerId, 'owner-A', 'the previous owner B must not decide terminal delivery');
      return expired
        ? { kind: 'missing' }
        : {
            kind: 'found',
            token: { ownerId, leaseGeneration: 9n },
            storeNow: new Date(0),
            leaseExpiresAt: new Date(10_000)
          };
    },
    readAuthority() {
      throw new Error('requester has no relocation authority registration');
    }
  };
  const node = {
    status: () => ({ routingId: sourceRid, lifecycleGeneration: 7n }),
    acceptRelocatedActorTerminal: (...args) => requester.acceptRelocatedActorTerminal(...args)
  };
  const receiver = new ZLinkHostServiceRelocationRuntime({
    meshNode: () => node,
    currentOwner: () => ({ ownerId: 'owner-A', leaseGeneration: 9n }),
    locationStore: () => store,
    liveDescriptors: async () => descriptors,
    actorTransfer: {
      relayCanonicalMaintenanceTerminal() {
        throw new Error('B reply capability was used');
      }
    }
  });
  const target = new ZLinkHostServiceRelocationRuntime({
    registration: { messageSerializers: undefined },
    locationStore: () => store,
    liveDescriptors: async () => descriptors
  });
  const relays = [];
  const acks = [];
  target.sendInfrastructureControl = async (_mesh, rid, parts) => {
    assert.equal(routingIds.routingIdsEqual(rid, sourceRid), true);
    const relay = wire.decodeMaintenanceReplyRelay(parts);
    relays.push(relay);
    await receiver.handleReplyRelay('mesh', relay, targetRid);
    return SubmitResult.Ok;
  };
  receiver.sendInfrastructureControl = async (_mesh, rid, bytes) => {
    assert.equal(rid, targetRid);
    const ack = wire.decodeMaintenanceReplyRelayAck(bytes);
    acks.push(ack);
    await target.acceptReplyRelayAck('mesh', ack, sourceRid);
    return SubmitResult.Ok;
  };
  const packet = {
    index: 0,
    header: Buffer.from(header).toString('base64'),
    payload: Buffer.from('{}').toString('base64'),
    returnResponse: true,
    source: {
      ownerId: 'owner-A',
      ownerLeaseGeneration: '9',
      nodeRid: String(sourceRid),
      ...(typeof sourceRid === 'string'
        ? {}
        : { nodeRidHex: routingIds.encodeRoutingIdStorageHex(sourceRid) }),
      nodeGeneration: '7',
      replyRouteId: pending.id.toString()
    },
    messageFollowContext: {
      operationId:
        operation.high.toString(16).padStart(16, '0') + operation.low.toString(16).padStart(16, '0')
    },
    messageFollowOrigin: {
      sourceNodeRid: sourceRid,
      originalOperation: operation,
      originalReplyRouteId: pending.id,
      requestSource: source
    }
  };
  const stage = {
    staging: { envelope: { aggregateId: '00000000-0000-0000-0000-000000000001' } },
    offer: { prepare: { targetAttemptGeneration: 1n } }
  };
  return {
    requester,
    pending,
    operation,
    source,
    packet,
    target,
    receiver,
    coordinator,
    stage,
    relays,
    acks,
    leaseReads
  };
}

for (const targetRid of ['C', 'D']) {
  test(`original requester A accepts the terminal from ${targetRid} after previous owner B ended`, async () => {
    const f = fixture(targetRid);
    try {
      await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: { answer: 42 }
      });
      const result = await f.pending.promise;
      assert.equal(result.terminalResult, RequestResult.Ok);
      const parts = multipart.decodeMultipartParts(
        result.payload.payload,
        (bytes) => Buffer.from(bytes),
        () => {}
      );
      assert.equal(protocol.decodeStreamHeader(parts[0]).requestSeq, 55n);
      assert.equal(protocol.decodeStreamHeader(parts[0]).correlationId, 'original-A');
      assert.deepEqual(JSON.parse(parts[1].toString()), { answer: 42 });
      assert.deepEqual(f.leaseReads, ['owner-A']);
      assert.deepEqual(f.acks[0].requestSource, f.source);
      assert.equal(f.acks[0].status, 'terminalReceived');
      await f.receiver.handleReplyRelay('mesh', f.relays[0], targetRid);
      assert.equal(f.acks[1].status, 'alreadyTerminal');
      await f.target.acceptReplyRelayAck('mesh', f.acks[0], 'A');
      assert.equal(f.target.pendingReplyRelays.size, 0);
      assert.equal(f.requester.pendingOperationCount, 0);
      assert.equal(
        f.requester.acceptRelocatedActorTerminal(
          { ...f.operation, low: f.operation.low + 1n },
          f.pending.id,
          result
        ),
        undefined
      );
      assert.equal(
        f.requester.acceptRelocatedActorTerminal(f.operation, f.pending.id + 1n, result),
        undefined
      );
    } finally {
      f.requester.close();
    }
  });
}

test('command 33 and 46 preserve the original requester binary RID', async () => {
  const rid = routingIds.decodeRoutingId('\ufffd', 'ff');
  const f = fixture('C', false, rid);
  void f.pending.promise.catch(() => {});
  try {
    await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
      index: 0,
      ok: true,
      response: { answer: 42 }
    });
    assert.equal((await f.pending.promise).terminalResult, RequestResult.Ok);
    assert.equal(routingIds.encodeRoutingIdStorageHex(f.acks[0].requestSource.nodeRid), 'ff');
    assert.equal(f.acks[0].status, 'terminalReceived');
    assert.equal(f.target.pendingReplyRelays.size, 0);
  } finally {
    f.requester.close();
  }
});

test('the target clears an expired original source lease without consulting B', async () => {
  const f = fixture('C', true);
  try {
    const delivery = await f.target.sendReplyRelay(
      'mesh',
      'A',
      {
        relocation: { high: 0n, low: 1n },
        operation: f.operation,
        replyRouteId: f.pending.id,
        coordinator: f.coordinator
      },
      f.source
    );
    assert.equal(delivery, 'sourceLeaseExpired');
    assert.deepEqual(f.leaseReads, ['owner-A']);
    assert.equal(f.relays.length, 0);
    assert.equal(f.target.pendingReplyRelays.size, 0);
  } finally {
    const closed = assert.rejects(f.pending.promise);
    f.requester.close();
    await closed;
  }
});

test('the target rejects an ACK whose original source generation changed', async () => {
  const f = fixture();
  let sent;
  const admission = new Promise((resolve) => {
    sent = resolve;
  });
  f.target.sendInfrastructureControl = async () => {
    sent();
    return SubmitResult.Ok;
  };
  const request = {
    relocation: { high: 0n, low: 1n },
    operation: f.operation,
    replyRouteId: f.pending.id,
    coordinator: f.coordinator,
    targetAttemptGeneration: 1n,
    participantId: 1n,
    sequence: 1n,
    terminalResult: RequestResult.Ok,
    failureCode: 0
  };
  const delivery = f.target.sendReplyRelay('mesh', 'A', request, f.source);
  const rejected = assert.rejects(delivery, /durable source fence/);
  try {
    await admission;
    await f.target.acceptReplyRelayAck(
      'mesh',
      {
        ...request,
        requestSource: { ...f.source, nodeGeneration: 8n },
        status: 'terminalReceived'
      },
      'A'
    );
    await rejected;
    assert.equal(f.target.pendingReplyRelays.size, 0);
  } finally {
    const closed = assert.rejects(f.pending.promise);
    f.requester.close();
    await closed;
  }
});

test('relocation terminal tombstones reuse the operation registry and expire on its existing scan', async () => {
  let now = 0;
  const registry = new OperationRegistry({ now: () => now });
  const pending = registry.register(10, 'registry', 15n);
  assert.equal(registry.complete(pending.id, 'reply', 100), true);
  assert.equal(await pending.promise, 'reply');
  assert.equal(registry.isPending(pending.id), false);
  assert.equal(registry.context(pending.id), 15n);
  assert.equal(registry.size, 0);
  assert.equal(registry.complete(pending.id, 'duplicate', 100), false);
  now = 101;
  registry.expire(now, 102);
  assert.equal(registry.context(pending.id), undefined);
});

for (const local of [false, true]) {
  test(`accepted Actor source keeps the same reference in the mailbox and dispatch origin (local=${local})`, async () => {
    let queued;
    let admitted;
    const admission = new Promise((resolve) => {
      admitted = resolve;
    });
    const job = { close() {} };
    const owner = { takeInitial: () => job, close() {} };
    const raw = {
      setServiceIngress() {},
      topology: { peer: () => ({ descriptor: { lifecycleGeneration: 7n } }) },
      mailbox: {
        tryEnqueue(record) {
          queued = record;
          admitted();
          return true;
        }
      },
      reserveLocalIngress: async () => owner
    };
    const runtime = new ServiceStatefulRuntime(raw, 'B', 8n);
    const actor = runtime.createActor('actor').ref;
    const source = {
      ownerId: local ? 'owner-B' : 'owner-A',
      leaseGeneration: 9n,
      nodeRid: local ? 'B' : 'A',
      nodeGeneration: local ? 8n : 7n
    };
    let reads = 0;
    runtime.setRequestSourceAcceptance(() => {
      reads++;
      return source;
    });
    try {
      if (local) {
        const pending = runtime.requestToActor(
          actor,
          { packetName: 'Probe', contentType: 'application/json', payload: Buffer.from('{}') },
          1000
        );
        await admission;
        assert.strictEqual(queued.requestSource, source);
        assert.strictEqual(queued.stateful.messageFollowOrigin.requestSource, source);
        assert.equal(queued.stateful.messageFollowOrigin.originalReplyRouteId, pending.id);
        queued.stateful.reply(RequestResult.Ok, 0);
        await pending.promise;
      } else {
        const ingress = {
          sourceRoutingId: 'A',
          requestSequence: 500n,
          command: wire.M6bServiceWireCommand.actorRequest,
          flags: 0,
          applicationJobOwner: owner,
          parts: [
            wire.encodeActorHeader(
              'actorRequest',
              runtime.tryActorFence(actor),
              91n,
              undefined,
              undefined,
              { high: 7n, low: 15n }
            ),
            multipart.encodeApplicationPayload({
              packetName: 'Probe',
              contentType: 'application/json',
              payload: Buffer.from('{}')
            })
          ]
        };
        assert.equal(await runtime.ingress(ingress), 'application');
        assert.strictEqual(queued.requestSource, source);
        assert.strictEqual(queued.stateful.messageFollowOrigin.requestSource, source);
        assert.equal(queued.stateful.messageFollowOrigin.originalReplyRouteId, 91n);
        assert.strictEqual(ingress.requestSource, source);
      }
      assert.equal(reads, 1);
    } finally {
      runtime.close();
    }
  });
}
