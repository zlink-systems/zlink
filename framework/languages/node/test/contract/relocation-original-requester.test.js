const { replyFixture } = require('./helpers/relocation-reply');
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const routingIds = require('../../packages/framework/dist/runtime/routing-id');
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/internal');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const multipart = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const protocol = require('../../packages/framework/dist/runtime/streams/protocol');
const {
  RequestResult,
  SubmitResult
} = require('../../packages/framework/dist/runtime/backend/runtime-values');

function deliver(runtime, sourceNodeRid, frames) {
  return runtime.tryHandleControl('mesh', {
    sourceNodeRid,
    parts: (Array.isArray(frames) ? frames : [frames]).map((frame) => ({
      data: () => Buffer.from(frame),
      close() {}
    }))
  });
}

async function fixture(targetRid = 'C', expired = false, sourceRid = 'A', flowEnabled = false) {
  let submission;
  const requester = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      isPeerRouteReady: () => true,
      peerLifecycleGeneration: (rid) => (rid === 'B' ? 8n : undefined),
      requestService(_rid, parts) {
        return new Promise((reply) => {
          submission = { parts, reply };
        });
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
  function startRequest() {
    const actor = { nodeRid: 'B', actorId: 'actor', generation: 1n };
    const pending = requester.requestToActor(
      actor,
      { packetName: 'Probe', contentType: 'application/json', payload: Buffer.from('{}') },
      1000,
      undefined,
      undefined,
      undefined,
      {
        actor,
        targetNodeGeneration: 8n,
        authorityOwnerGeneration: 1n,
        ownerLeaseGeneration: 3n
      }
    );
    const { parts, reply } = submission;
    return {
      pending,
      operation: wire.decodeStatefulHeader(parts[0]).operation,
      replyDirectly: () => reply([wire.encodeStatefulReply(pending.id, RequestResult.Ok, 0)])
    };
  }
  const { pending, operation, replyDirectly } = startRequest();
  const source = {
    ownerId: 'owner-A',
    leaseGeneration: 9n,
    nodeRid: sourceRid,
    nodeGeneration: 7n
  };
  const requesterOwner = { ownerId: source.ownerId, leaseGeneration: source.leaseGeneration };
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
    currentOwner: () => requesterOwner,
    locationStore: () => store,
    liveDescriptors: async () => descriptors,
    actorTransfer: {
      relayCanonicalMaintenanceTerminal() {
        return 'alreadyTerminal';
      }
    }
  });
  const replies = replyFixture({
    ownerId: coordinator.ownerId,
    leaseGeneration: coordinator.leaseGeneration
  });
  await replies.setWriter(replies.writer);
  const targetStore = { ...store, relocationReplies: replies.store };
  const targetOptions = {
    registration: { messageSerializers: undefined },
    meshNode: () => targetNode,
    flowCreationEnabled: () => flowEnabled,
    currentOwner: () => ({
      ownerId: coordinator.ownerId,
      leaseGeneration: coordinator.leaseGeneration
    }),
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
    assert.equal((await replies.rows())[0]?.state, 'pending', 'reply must follow result CAS');
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
        participants: [{ key: 'zla1:a:5:actor' }]
      }
    },
    offer: { prepare: { targetAttemptGeneration: 1n } }
  };
  await replies.seed({
    relocationId: stage.staging.envelope.aggregateId,
    objectKey: stage.staging.envelope.participants[0].key,
    operationId: packet.messageFollowContext.operationId,
    replyRouteId: packet.source.replyRouteId,
    source: {
      ownerId: source.ownerId,
      leaseGeneration: source.leaseGeneration.toString(),
      nodeRid: routingIds.encodeRoutingIdStorageHex(sourceRid),
      nodeGeneration: source.nodeGeneration.toString()
    }
  });
  return {
    requester,
    requesterOwner,
    startRequest,
    replyDirectly,
    replies,
    targetStore,
    targetOptions,
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
    const f = await fixture('C', false, 'A', enabled);
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
    const f = await fixture(targetRid);
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

test('a relay after a direct result is discarded and acknowledged as alreadyTerminal', async () => {
  const f = await fixture();
  try {
    f.replyDirectly();
    const direct = await f.pending.promise;
    await deliver(
      f.receiver,
      'C',
      wire.encodeMaintenanceReplyRelay({
        relocation: { high: 0n, low: 1n },
        operation: f.operation,
        replyRouteId: f.pending.id,
        coordinator: f.coordinator,
        targetAttemptGeneration: 1n,
        participantId: 1n,
        sequence: 1n,
        terminalResult: RequestResult.TimedOut,
        failureCode: 0
      })
    );
    assert.equal(direct.terminalResult, RequestResult.Ok);
    assert.equal(f.acks[0].status, 'alreadyTerminal');
    assert.deepEqual(f.acks[0].requestSource, f.source);
    assert.equal(f.requester.pendingOperationCount, 0);
  } finally {
    await f.target.dispose();
    f.requester.close();
  }
});

test('an unauthenticated coordinator gets no ACK for an unknown operation', async () => {
  const f = await fixture();
  try {
    f.replyDirectly();
    await f.pending.promise;
    await assert.rejects(
      deliver(
        f.receiver,
        'C',
        wire.encodeMaintenanceReplyRelay({
          relocation: { high: 0n, low: 1n },
          operation: f.operation,
          replyRouteId: f.pending.id,
          coordinator: { ...f.coordinator, leaseGeneration: f.coordinator.leaseGeneration + 1n },
          targetAttemptGeneration: 1n,
          participantId: 1n,
          sequence: 1n,
          terminalResult: RequestResult.Ok,
          failureCode: 0
        })
      ),
      /not the exact admitted source fence/
    );
    assert.equal(f.acks.length, 0);
  } finally {
    f.requester.close();
  }
});

test('command 33 and 46 preserve the original requester binary RID', async () => {
  const rid = routingIds.decodeRoutingId('\ufffd', 'ff');
  const f = await fixture('C', false, rid);
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
  const f = await fixture('C', true);
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
  const f = await fixture();
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

test('a current requester ACK leaves an earlier lease row pending', async () => {
  const f = await fixture();
  f.replyDirectly();
  await f.pending.promise;
  f.requesterOwner.leaseGeneration += 1n;

  const send = f.targetNode.sendInfrastructureControlFrames;
  let reportFirstDelivery;
  const firstDelivery = new Promise((resolve) => {
    reportFirstDelivery = resolve;
  });
  f.targetNode.sendInfrastructureControlFrames = async (rid, parts) => {
    try {
      await send(rid, parts);
      reportFirstDelivery({});
    } catch (error) {
      reportFirstDelivery({ error });
    }
    return SubmitResult.Ok;
  };
  const completion = f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
    index: 0,
    ok: true,
    response: { answer: 42 }
  });

  try {
    const { error } = await Promise.race([firstDelivery, completion.then(() => ({}))]);
    assert.equal(f.acks[0]?.status, 'alreadyTerminal');
    assert.deepEqual(f.acks[0].requestSource, {
      ...f.source,
      leaseGeneration: f.requesterOwner.leaseGeneration
    });
    assert.match(error?.message ?? '', /does not match its durable source fence/);
    assert.equal(f.target[inspectRuntime]().pendingReplyRelays, 1);
    assert.equal((await f.replies.rows())[0].state, 'pending');
    f.targetStore.readOwnerLease = async () => ({ kind: 'missing' });
    await completion;
    assert.equal((await f.replies.rows())[0].state, 'sourceLeaseExpired');
  } finally {
    await f.target.dispose();
    await completion.catch(() => {});
    f.requester.close();
  }
});

test('the requester registry tracks only requests awaiting direct or relayed results', async () => {
  const f = await fixture();
  const direct = [f.startRequest(), f.startRequest()];
  const results = Promise.all([f.pending.promise, ...direct.map(({ pending }) => pending.promise)]);
  try {
    assert.equal(f.requester.pendingOperationCount, 3);
    for (const request of direct) request.replyDirectly();
    await Promise.all(direct.map(({ pending }) => pending.promise));
    assert.equal(f.requester.pendingOperationCount, 1);
    await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
      index: 0,
      ok: true,
      response: { answer: 42 }
    });
    assert.equal((await results).length, 3);
    assert.equal(f.requester.pendingOperationCount, 0);
    assert.equal(f.requester.nextMaintenanceDeadlineMs(), Infinity);
  } finally {
    await f.target.dispose();
    f.requester.close();
    await results.catch(() => {});
  }
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

test('result CAS rejection forbids reply and leaves the row accepted', async () => {
  const f = await fixture();
  f.replies.provider.write = async () => {
    throw new TypeError('injected result CAS failure');
  };
  try {
    await assert.rejects(
      f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: {}
      }),
      /injected result CAS failure/
    );
    assert.equal((await f.replies.rows())[0].state, 'accepted');
    assert.equal(f.relays.length, 0);
  } finally {
    void f.pending.promise.catch(() => {});
    f.requester.close();
  }
});

test('lost ACK CAS response rereads the terminal row without a second relay', async () => {
  const f = await fixture();
  const write = f.replies.provider.write.bind(f.replies.provider);
  let lost = 0;
  f.replies.provider.write = async (batch, signal) => {
    const result = await write(batch, signal);
    if (JSON.parse(Buffer.from(batch.mutations[0].bytes)).state === 'terminalReceived') {
      lost++;
      throw new Error('injected lost ACK CAS response');
    }
    return result;
  };
  try {
    await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
      index: 0,
      ok: true,
      response: { answer: 42 }
    });
    assert.equal(lost, 1);
    assert.equal(f.relays.length, 1);
    assert.equal((await f.replies.rows())[0].state, 'terminalReceived');
  } finally {
    await f.target.dispose();
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
    const f = await fixture();
    f.targetStore.readOwnerLease = async () => lease;
    try {
      await f.target.relayTerminalReply('mesh', f.stage, f.coordinator, 1n, f.packet, {
        index: 0,
        ok: true,
        response: {}
      });
      assert.equal((await f.replies.rows())[0].state, 'sourceLeaseExpired');
      assert.equal(f.relays.length, 0);
    } finally {
      void f.pending.promise.catch(() => {});
      f.requester.close();
    }
  });

test('Relocate deadline ends the caller while durable terminal delivery continues', async () => {
  const f = await fixture();
  const deadline = new AbortController();
  deadline.abort(new Error('Relocate deadline'));
  try {
    await f.target.relayTerminalReply(
      'mesh',
      f.stage,
      f.coordinator,
      1n,
      f.packet,
      { index: 0, ok: true, response: { answer: 42 } },
      deadline.signal
    );
    assert.equal((await f.pending.promise).terminalResult, RequestResult.Ok);
    assert.equal((await f.replies.rows())[0].state, 'terminalReceived');
  } finally {
    void f.pending.promise.catch(() => {});
    f.requester.close();
  }
});
