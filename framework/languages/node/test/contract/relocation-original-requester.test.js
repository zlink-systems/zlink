const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const routingIds = require('../../packages/framework/dist/runtime/routing-id');
const assert = require('node:assert/strict');
const test = require('node:test');
const deferred = require('./helpers/deferred.cjs');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ZLinkFrameworkRelocationMode,
  ZLinkFrameworkRuntimeHost,
  ZLinkHostServiceRelocationRuntime,
  createFrameworkRegistrationWithBuilder
} = require('../../packages/framework/dist/internal');
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

function deliver(runtime, sourceNodeRid, frames) {
  return runtime.tryHandleControl('mesh', {
    sourceNodeRid,
    parts: (Array.isArray(frames) ? frames : [frames]).map((frame) => ({
      data: () => Buffer.from(frame),
      close() {}
    }))
  });
}

function fixture(targetRid = 'C', expired = false, sourceRid = 'A', flowEnabled = false) {
  let submitted;
  const requester = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      isPeerRouteReady: () => true,
      peerLifecycleGeneration: (rid) => (rid === 'B' ? 8n : undefined),
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
    sendInfrastructureControl: (rid, bytes) => sendAck(rid, bytes),
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
  const blobBytes = new Map();
  let authority = {
    kind: 'snapshot',
    storeVersion: { value: '1' },
    payload: Buffer.from('application'),
    ownerId: coordinator.ownerId,
    ownerLeaseGeneration: coordinator.leaseGeneration,
    relocation: {
      relocationId: '00000000-0000-0000-0000-000000000001',
      sourceInventorySha256: 'ab'.repeat(32),
      completion: {
        acceptedRequests: 1n,
        completedResults: 0n,
        pendingDeliveries: 0n,
        reference: null,
        sha256: null
      }
    }
  };
  const targetStore = {
    ...store,
    async readAuthority() {
      return authority;
    },
    async compareExchangeAuthority(_key, version, mutation) {
      assert.equal(version.value, authority.storeVersion.value);
      authority = {
        ...authority,
        ...mutation,
        kind: 'snapshot',
        storeVersion: { value: String(Number(version.value) + 1) }
      };
      return { ...authority, kind: 'stored' };
    }
  };
  const relocationStore = {
    async put(reference, bytes, retention) {
      blobBytes.set(reference.value, Buffer.from(bytes));
      return { kind: 'stored', storeNow: new Date(0), expiresAt: new Date(retention) };
    },
    async read(reference) {
      return blobBytes.has(reference.value)
        ? {
            kind: 'found',
            bytes: blobBytes.get(reference.value),
            storeNow: new Date(0),
            expiresAt: new Date(86400000)
          }
        : { kind: 'missing', storeNow: new Date(0) };
    }
  };
  const targetOptions = {
    registration: { messageSerializers: undefined },
    meshNode: () => targetNode,
    flowCreationEnabled: () => flowEnabled,
    currentOwner: () => ({
      ownerId: coordinator.ownerId,
      leaseGeneration: coordinator.leaseGeneration
    }),
    relocationStore,
    locationStore: () => targetStore,
    liveDescriptors: async () => descriptors
  };
  const target = new ZLinkHostServiceRelocationRuntime(targetOptions);
  const relays = [];
  const acks = [];
  const targetNode = {
    sendInfrastructureControl() {
      throw new Error('terminal relay requires multipart frames');
    },
    sendInfrastructureControlFrames: (rid, parts) => sendRelay(rid, parts)
  };
  const sendRelay = async (rid, parts) => {
    assert.equal(routingIds.routingIdsEqual(rid, sourceRid), true);
    assert.equal(
      authority.relocation.completion.completedResults,
      1n,
      'reply must follow completion CAS'
    );
    const relay = wire.decodeMaintenanceReplyRelay(parts);
    relays.push(relay);
    await deliver(receiver, targetRid, parts);
    return SubmitResult.Ok;
  };
  const sendAck = async (rid, bytes) => {
    assert.equal(rid, targetRid);
    const ack = wire.decodeMaintenanceReplyRelayAck(bytes);
    acks.push(ack);
    await deliver(target, sourceRid, bytes);
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
    lane: Promise.resolve(),
    staging: {
      envelope: {
        aggregateId: '00000000-0000-0000-0000-000000000001',
        participants: [{ key: 'zla1:a:actor' }]
      }
    },
    offer: { prepare: { targetAttemptGeneration: 1n } }
  };
  return {
    requester,
    currentAuthority: () => authority,
    targetStore,
    targetOptions,
    relocationStore,
    targetNode,
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

for (const mode of ['off', 'errors', 'normal', 'detailed']) {
  test(`relocated Actor reply respects current tracing mode ${mode}`, async () => {
    const enabled = mode === 'normal' || mode === 'detailed';
    const f = fixture('C', false, 'A', enabled);
    const requestHeader = protocol.decodeStreamHeader(Buffer.from(f.packet.header, 'base64'));
    f.packet.header = Buffer.from(
      protocol.encodeStreamHeader({
        ...requestHeader,
        flowId: '01900000-0000-7000-8000-000000000001',
        flowOrigin: 'Inbound'
      })
    ).toString('base64');
    try {
      await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: { answer: 42 }
      });
      const result = await f.pending.promise;
      const parts = multipart.decodeMultipartParts(
        result.payload.payload,
        (bytes) => Buffer.from(bytes),
        () => {}
      );
      assert.equal(parts.length, 1);
      const header = protocol.decodeStreamHeader(protocol.decodeStreamFrame(parts[0]).header);
      assert.equal(header.flowId, enabled ? '01900000-0000-7000-8000-000000000001' : undefined);
      assert.equal(header.flowOrigin, enabled ? 'Inbound' : undefined);
      assert.equal(header.correlationId, 'original-A');
      assert.equal(header.requestSeq, 55n);
    } finally {
      f.requester.close();
    }
  });
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
      assert.equal(parts.length, 1);
      const frame = protocol.decodeStreamFrame(parts[0]);
      const header = protocol.decodeStreamHeader(frame.header);
      assert.equal(header.requestSeq, 55n);
      assert.equal(header.correlationId, 'original-A');
      assert.deepEqual(JSON.parse(Buffer.from(frame.payload).toString()), { answer: 42 });
      assert.deepEqual(f.leaseReads, ['owner-A']);
      assert.deepEqual(f.acks[0].requestSource, f.source);
      assert.equal(f.acks[0].status, 'terminalReceived');
      await deliver(f.receiver, targetRid, wire.encodeMaintenanceReplyRelay(f.relays[0]));
      assert.equal(f.acks[1].status, 'alreadyTerminal');
      await deliver(f.target, 'A', wire.encodeMaintenanceReplyRelayAck(f.acks[0]));
      assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 0);
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
    assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 0);
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
    assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 0);
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
  f.targetNode.sendInfrastructureControlFrames = async () => {
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

  try {
    await admission;
    await assert.rejects(
      deliver(
        f.target,
        'A',
        wire.encodeMaintenanceReplyRelayAck({
          ...request,
          requestSource: { ...f.source, nodeGeneration: 8n },
          status: 'terminalReceived'
        })
      ),
      /durable source fence/
    );
    assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 1);
    await deliver(
      f.target,
      'A',
      wire.encodeMaintenanceReplyRelayAck({
        ...request,
        requestSource: f.source,
        status: 'terminalReceived'
      })
    );
    assert.equal(await delivery, 'terminalReceived');
    assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 0);
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
    let ingressHandler;
    const raw = {
      setServiceIngress(handler) {
        ingressHandler = handler;
      },
      topology: { peer: () => ({ descriptor: { lifecycleGeneration: 7n } }) },
      peerLifecycleGeneration: () => 7n,
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
        assert.equal(reads, 0);
        assert.equal(
          await queued.sourcePreparationOwner.prepareDispatch(queued, new AbortController().signal),
          true
        );
        assert.strictEqual(queued.requestSource, source);
        assert.strictEqual(queued.stateful.messageFollowOrigin.requestSource, source);
        assert.equal(queued.stateful.messageFollowOrigin.originalReplyRouteId, pending.id);
        queued.stateful.reply(RequestResult.Ok, 0);
        await pending.promise;
      } else {
        const ingress = {
          sourceRoutingId: 'A',
          sourceNodeGeneration: 7n,
          requestSequence: 500n,
          command: wire.M6bServiceWireCommand.actorRequest,
          flags: 0,
          applicationJobOwner: owner,
          parts: [
            wire.encodeActorHeader(
              'actorRequest',
              {
                actor,
                targetNodeGeneration: 8n,
                authorityOwnerGeneration: 1n,
                ownerLeaseGeneration: 1n
              },
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
        assert.equal(await ingressHandler(ingress), 'application');
        assert.equal(reads, 0);
        assert.equal(
          await queued.sourcePreparationOwner.prepareDispatch(queued, new AbortController().signal),
          true
        );
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

test('completion Put rejection forbids both authority CAS and reply', async () => {
  const f = fixture();
  let cas = 0;
  f.targetStore.compareExchangeAuthority = async () => {
    cas++;
    throw new Error('CAS must not run');
  };
  f.relocationStore.put = async () => {
    throw new TypeError('injected Put failure');
  };
  try {
    await assert.rejects(
      f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: {}
      }),
      /injected Put failure/
    );
    assert.equal(cas, 0);
    assert.equal(f.relays.length, 0);
  } finally {
    void f.pending.promise.catch(() => {});
    f.requester.close();
  }
});

test('the same target runtime redelivers stored bytes after relay ACK storage failed', async () => {
  const f = fixture();
  const original = f.targetStore.compareExchangeAuthority;
  f.targetStore.compareExchangeAuthority = async (...args) => {
    if (args[2].relocation.completion.pendingDeliveries === 0n)
      throw new TypeError('injected ACK storage failure');
    return original(...args);
  };
  try {
    await assert.rejects(
      f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: { answer: 42 }
      }),
      /injected ACK storage failure/
    );
    assert.equal(f.currentAuthority().relocation.completion.pendingDeliveries, 1n);
    const first = f.relays[0];
    f.targetNode.status = () => ({ routingId: 'C', lifecycleGeneration: 11n });
    f.targetStore.compareExchangeAuthority = original;
    const current = await f.target.redeliverPendingCompletions(
      'mesh',
      { value: 'zla1:a:actor' },
      f.currentAuthority()
    );
    assert.equal(current.relocation.completion.pendingDeliveries, 0n);
    assert.equal(f.relays.length, 2);
    assert.deepEqual(f.relays[1].payload, first.payload);
    assert.deepEqual(f.relays[1].operation, first.operation);
    assert.equal(f.acks[1].status, 'alreadyTerminal');
    await f.target.redeliverPendingCompletions('mesh', { value: 'zla1:a:actor' }, current);
    assert.equal(f.relays.length, 2);
  } finally {
    f.target.dispose();
    void f.pending.promise.catch(() => {});
    f.requester.close();
  }
});

for (const lease of [
  { kind: 'missing' },
  {
    kind: 'found',
    token: { ownerId: 'owner-A', leaseGeneration: 10n },
    storeNow: new Date(0),
    leaseExpiresAt: new Date(1000)
  },
  {
    kind: 'found',
    token: { ownerId: 'owner-A', leaseGeneration: 9n },
    storeNow: new Date(1000),
    leaseExpiresAt: new Date(1000)
  }
])
  test(`exact requester lease terminal: ${lease.kind}-${lease.token?.leaseGeneration}`, async () => {
    const f = fixture();
    f.targetStore.readOwnerLease = async () => lease;
    try {
      await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: {}
      });
      assert.equal(f.currentAuthority().relocation.completion.pendingDeliveries, 0n);
      assert.equal(f.relays.length, 0);
    } finally {
      void f.pending.promise.catch(() => {});
      f.requester.close();
    }
  });

test('Relocate deadline ends the caller while durable terminal delivery continues', async () => {
  const f = fixture();
  const registration = createFrameworkRegistrationWithBuilder(() => {});
  const host = new ZLinkFrameworkRuntimeHost({ registration });
  const mode = ZLinkFrameworkRelocationMode.PlannedMaintenance;
  const deadline = new AbortController();
  const { promise: lane, resolve: release } = deferred();
  f.stage.lane = lane;
  const delivery = f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
    index: 0,
    ok: true,
    response: { answer: 42 }
  });
  host.relocationOperationKey = `${mode}:${registration.applicationVersion}`;
  host.relocationOperation = delivery;
  try {
    const waiting = host.relocate({ mode, signal: deadline.signal });
    const reason = new Error('Relocate deadline');
    deadline.abort(reason);
    await assert.rejects(waiting, (error) => error === reason);
    assert.equal(f.relays.length, 0);
    release();
    await delivery;
    assert.equal((await f.pending.promise).terminalResult, RequestResult.Ok);
    assert.equal(f.currentAuthority().relocation.completion.pendingDeliveries, 0n);
  } finally {
    release();
    void f.pending.promise.catch(() => {});
    f.requester.close();
  }
});
