const assert = require('node:assert/strict');
const test = require('node:test');
const { RequestResult } = require('@zlink-systems/zlink');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const wire = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  encodeApplicationPayload,
  M6aServiceWireCommand
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
const {
  SERVICE_WIRE_COMMAND_OFFSET,
  SERVICE_WIRE_FLAGS_OFFSET
} = require('../../packages/framework/dist/runtime/foundation/service-wire-binary-primitives');
const {
  encodeServiceMetadataFrame
} = require('../../packages/framework/dist/runtime/foundation/service-metadata-codec');
const {
  ZLinkDispatchErrorReporter
} = require('../../packages/framework/dist/runtime/channels/dispatch-error-reporter');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');
const fs = require('node:fs');
const flowRecords = [];
const TEST_APPLICATION_JOB_CAPACITY = 1n;
const TEST_NATIVE_REQUEST_SEQUENCE = 1n;
const loggerProvider = new LoggerProvider({
  processors: [
    {
      onEmit(record) {
        flowRecords.push(record);
        if (process.env.ZLINK_TEST_FLOW_FILE)
          fs.appendFileSync(
            process.env.ZLINK_TEST_FLOW_FILE,
            JSON.stringify(record.attributes, (_key, value) =>
              typeof value === 'bigint' ? String(value) : value
            ) + '\n'
          );
      },
      forceFlush: async () => {},
      shutdown: async () => {}
    }
  ]
});
logs.setGlobalLoggerProvider(loggerProvider);
test.after(() => loggerProvider.shutdown());

function ingressOwner() {
  const permit = {
    markApplicationQueued() {},
    detachForHandlerTurn() {},
    releaseBeforeHandler() {},
    releaseAfterInternalProcessing() {}
  };
  return ApplicationIngressRecordOwner.create({ acquire: async () => permit }, permit, {
    close() {}
  });
}

// nativeReplyIngress models a Core Request record carrying its live reply capability.
function pair({ nativeReplyIngress = false } = {}) {
  const nodes = new Map();
  const events = [];
  function node(rid, generation) {
    const queued = [];
    const sent = [];
    const replyDeliveries = [];
    const applicationJobQueue = new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration(
        { maxQueuedApplicationJobs: TEST_APPLICATION_JOB_CAPACITY },
        () => TEST_APPLICATION_JOB_CAPACITY
      )
    );
    let ingress;
    let connected = true;
    const disconnect = new Set();
    const raw = {
      topology: {
        peer: (peer) =>
          connected && nodes.has(peer)
            ? { descriptor: { lifecycleGeneration: nodes.get(peer).generation } }
            : undefined
      },
      observePeerConnectionIntentRemoved(callback) {
        disconnect.add(callback);
        return () => disconnect.delete(callback);
      },
      setServiceIngress(handler) {
        ingress = handler;
      },
      reserveLocalIngress: async () => ingressOwner(),
      mailbox: {
        tryEnqueue(record) {
          queued.push(record);
          return true;
        }
      },
      async sendService(target, parts) {
        sent.push({ target, parts });
        events.push(`send:${rid}:${parts[0][SERVICE_WIRE_COMMAND_OFFSET]}`);
        const destination = nodes.get(target);
        const received = { sourceRoutingId: rid, parts };
        if (
          nativeReplyIngress &&
          parts[0][SERVICE_WIRE_COMMAND_OFFSET] === wire.M6bServiceWireCommand.instanceSpot
        ) {
          const request = wire.decodeStatefulHeader(parts[0]);
          if (
            request.kind === 'instanceSpot' &&
            request.operationKind === 'request' &&
            request.replyRouteId !== undefined
          ) {
            received.requestSequence = TEST_NATIVE_REQUEST_SEQUENCE;
            received.reply = (replyParts) => {
              replyDeliveries.push(
                nodes.get(rid).receive({ sourceRoutingId: target, parts: replyParts })
              );
            };
          }
        }
        const permit = await destination.applicationJobQueue.acquire();
        try {
          await destination.receive(received);
          return true;
        } finally {
          permit.releaseAfterInternalProcessing();
        }
      },
      requestService() {
        throw new Error('Cold request must use send admission and its source pending operation.');
      },
      replyService(record, parts) {
        if (record.requestSequence === undefined || record.reply === undefined)
          throw new Error('Native reply requires the original request capability.');
        events.push(`reply:${rid}:${parts[0][SERVICE_WIRE_COMMAND_OFFSET]}`);
        record.reply(parts);
      }
    };
    const runtime = new ServiceStatefulRuntime(raw, rid, generation);
    const reporter = new ZLinkDispatchErrorReporter(
      undefined,
      undefined,
      { reportRuntimeTaskException() {} },
      {
        diagnostics: { messageFlow: 'normal', sampleRate: 1, includeMessageSizes: false },
        liveMode: { mode: 'normal' },
        sourceMeshGeneration: generation
      }
    );
    runtime.setDispatchErrorReporter(reporter, 'play');
    const value = {
      generation,
      runtime,
      queued,
      sent,
      replyDeliveries,
      applicationJobQueue,
      raw,
      reporter,
      receive: (record) =>
        ingress({
          command: record.parts[0][SERVICE_WIRE_COMMAND_OFFSET],
          flags: record.parts[0][SERVICE_WIRE_FLAGS_OFFSET],
          applicationJobOwner: ingressOwner(),
          ...record
        }),
      disconnect(peer) {
        connected = false;
        for (const listener of disconnect) listener(peer);
      },
      observerCount: () => disconnect.size,
      hidePeer() {
        connected = false;
      },
      async occupyAllApplicationPermits() {
        const permits = [];
        const limit = applicationJobQueue.snapshot().effectiveMaxQueuedApplicationJobs;
        while (BigInt(permits.length) < limit) permits.push(await applicationJobQueue.acquire());
        let released = false;
        return () => {
          if (released) return;
          released = true;
          for (const permit of permits) permit.releaseAfterInternalProcessing();
        };
      }
    };
    nodes.set(rid, value);
    return value;
  }
  const source = node('source', 5n);
  let target = node('target', 7n);
  const placement = {
    targetNodeRid: 'target',
    targetNodeGeneration: 7n,
    targetSpotId: 'room',
    stableType: 'T',
    descriptorVersion: '1'
  };
  const route = {
    ...placement,
    objectGeneration: 3n,
    authorityOwnerGeneration: 4n,
    ownerId: 'target',
    leaseGeneration: 1n,
    storeVersion: 'ready'
  };
  const authority = {
    read: async () => ({ kind: 'missing' }),
    reserve: async () => ({
      kind: 'reserved',
      reservation: { attempt: 3n, authorityOwnerGeneration: 4n, token: 'reservation' }
    }),
    commit: async () => ({ kind: 'committed', route }),
    complete: async () => {
      events.push('durable:complete');
      return route;
    },
    abort: async () => assert.fail('Successful activation must not abort')
  };
  target.runtime.registerAsyncInstanceActivationAuthority(authority);
  const payload = {
    packetName: 'Lookup',
    contentType: 'application/json',
    payload: Buffer.from('{"value":1}')
  };
  return {
    source,
    get target() {
      return target;
    },
    placement,
    route,
    payload,
    events,
    restartTarget() {
      target.runtime.close();
      target = node('target', 7n);
      target.runtime.registerAsyncInstanceActivationAuthority(authority);
    },
    close() {
      source.runtime.close();
      target.runtime.close();
    }
  };
}

for (const recovery of [false, true]) {
  for (const terminal of [RequestResult.Ok, RequestResult.Busy]) {
    test(`cold ${recovery ? 'recovered' : 'normal'} reply completes source once (terminal=${terminal})`, async (t) => {
      const p = pair();
      t.after(() => p.close());
      const metadataFrame = encodeServiceMetadataFrame(new Map([['test', 'first']]));
      const pending = p.source.runtime.requestToMissingInstanceSpot(
        p.placement,
        p.payload,
        10000,
        'origin',
        metadataFrame
      );
      pending.promise.catch(() => {});
      await new Promise((resolve) => setImmediate(resolve));
      assert.equal(p.source.sent.length, 1);
      const request = wire.decodeStatefulHeader(p.source.sent[0].parts[0]);
      assert.equal(request.kind, 'instanceSpot');
      assert.equal(p.target.queued.length, 1);
      if (recovery) {
        p.target.queued.pop().applicationJob.close();
        // Target recovery carries a logical route distinct from OperationId.low.
        const envelope = {
          targetMeshName: 'play',
          target: p.placement,
          sourceNodeRid: 'source',
          sourceNodeGeneration: 5n,
          sourceSpotId: 'origin',
          operationKind: 'request',
          operation: request.operation,
          replyRouteId: pending.id,
          deadlineUnixMs: request.deadlineUnixMs,
          applicationPayloadFrame: encodeApplicationPayload(p.payload),
          metadataFrame
        };
        // A new runtime has no live ingress capability or admitted-operation memory.
        p.restartTarget();
        await p.target.runtime.recoverInstanceActivation(envelope, p.route);
      }
      const mailbox = p.target.queued.shift();
      assert.equal(mailbox.stateful.sourceSpotId, 'origin');
      assert.deepEqual(mailbox.stateful.applicationMetadata, metadataFrame);
      assert.equal(mailbox.stateful.deadlineUnixMs, request.deadlineUnixMs);
      assert.deepEqual(mailbox.parts[1], encodeApplicationPayload(p.payload));
      mailbox.stateful.onHandlerTurnStarted?.();
      mailbox.stateful.reply(terminal, 0, terminal === RequestResult.Ok ? p.payload : undefined);
      await mailbox.stateful.onTerminalCompletion();
      mailbox.applicationJob.close();
      const result = await pending.promise;
      assert.equal(result.terminalResult, terminal);
      if (terminal === RequestResult.Ok)
        assert.deepEqual(result.payload.payload, p.payload.payload);
      assert.equal(p.source.runtime.pendingOperationCount, 0);
      assert.equal(p.source.observerCount(), 0);
      assert.equal(p.target.sent.length, 1);
      assert.ok(
        p.events.indexOf('durable:complete') <
          p.events.indexOf(`send:target:${M6aServiceWireCommand.reply}`)
      );
      assert.equal(
        await p.source.receive({ sourceRoutingId: 'target', parts: p.target.sent[0].parts }),
        'infrastructure'
      );
      assert.equal(p.source.runtime.pendingOperationCount, 0);
    });
  }
}

test('normal cold reply uses the original reply capability when source application permits are full', async (t) => {
  const p = pair({ nativeReplyIngress: true });
  let releasePermits;
  t.after(async () => {
    releasePermits?.();
    await new Promise((resolve) => setImmediate(resolve));
    p.close();
  });
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  pending.promise.catch(() => {});
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(p.source.sent.length, 1);
  assert.equal(p.target.queued.length, 1);
  releasePermits = await p.source.occupyAllApplicationPermits();
  const full = p.source.applicationJobQueue.snapshot();
  assert.equal(full.permitsInUse, full.effectiveMaxQueuedApplicationJobs);

  try {
    const mailbox = p.target.queued.shift();
    mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
    await mailbox.stateful.onTerminalCompletion();
    mailbox.applicationJob.close();
    await Promise.all(p.source.replyDeliveries);

    const outcome = await Promise.race([
      pending.promise.then((result) => ({ kind: 'completed', result })),
      new Promise((resolve) => setImmediate(() => resolve({ kind: 'blocked' })))
    ]);
    assert.equal(outcome.kind, 'completed');
    assert.equal(outcome.result.terminalResult, RequestResult.Ok);
    assert.equal(p.source.runtime.pendingOperationCount, 0);
    assert.equal(p.source.applicationJobQueue.snapshot().permitsInUse, full.permitsInUse);
    assert.equal(p.target.sent.length, 0);
    assert.ok(
      p.events.indexOf('durable:complete') <
        p.events.indexOf(`reply:target:${M6aServiceWireCommand.reply}`)
    );
  } finally {
    releasePermits();
    await new Promise((resolve) => setImmediate(resolve));
  }
});

test('cold reply without a current source peer is not sent; original source deadline wins', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  const rejected = assert.rejects(pending.promise, /timed out/);
  await new Promise((resolve) => setImmediate(resolve));
  p.target.hidePeer();
  const mailbox = p.target.queued.shift();
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  assert.equal(p.target.sent.length, 0);
  assert.equal(p.target.reporter.reportedCount, 1);
  assert.equal(p.source.runtime.pendingOperationCount, 1);
  p.source.runtime.expireOperations(performance.now() + 20000, performance.now() + 1000);
  await rejected;
});

for (const winner of ['timeout', 'cancellation', 'disconnect', 'close']) {
  test(`cold late reply preserves ${winner} terminal`, async (t) => {
    const p = pair();
    t.after(() => p.close());
    const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
    let completions = 0;
    const rejected = assert.rejects(pending.promise.finally(() => completions++));
    await new Promise((resolve) => setImmediate(resolve));
    if (winner === 'timeout')
      p.source.runtime.expireOperations(performance.now() + 20000, performance.now() + 1000);
    if (winner === 'cancellation') p.source.runtime.operations.cancel(pending.id);
    if (winner === 'disconnect') p.source.disconnect('target');
    if (winner === 'close') p.source.runtime.close();
    await rejected;
    const mailbox = p.target.queued.shift();
    mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
    await mailbox.stateful.onTerminalCompletion();
    mailbox.applicationJob.close();
    await new Promise((resolve) => setImmediate(resolve));
    assert.equal(completions, 1);
    assert.equal(p.source.runtime.pendingOperationCount, 0);
  });
}

test('cold source accepts only its target RID and lifecycle; malformed reply fails the original pending request', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  const rejected = assert.rejects(
    pending.promise,
    (error) => error.kind === ZLinkFrameworkErrorKind.ProtocolError
  );
  await new Promise((resolve) => setImmediate(resolve));
  const parts = [wire.encodeStatefulReply(pending.id, RequestResult.Ok, 0)];
  assert.equal(await p.source.receive({ sourceRoutingId: 'other', parts }), 'protocolError');
  const originalPeer = p.source.raw.topology.peer;
  p.source.raw.topology.peer = () => ({ descriptor: { lifecycleGeneration: 8n } });
  assert.equal(await p.source.receive({ sourceRoutingId: 'target', parts }), 'protocolError');
  assert.equal(p.source.runtime.pendingOperationCount, 1);
  p.source.raw.topology.peer = originalPeer;
  assert.equal(
    await p.source.receive({ sourceRoutingId: 'target', parts: [...parts, Buffer.of(0)] }),
    'infrastructure'
  );
  await rejected;
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  p.target.queued.shift().applicationJob.close();
});

test('cold send admission rejection fails the same source pending request', async (t) => {
  const p = pair();
  t.after(() => p.close());
  p.source.raw.sendService = async () => false;
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  await assert.rejects(pending.promise, /admission failed/);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  assert.equal(p.source.observerCount(), 0);
  assert.equal(p.target.queued.length, 0);
});

test('cold pending requests each release their intent observer on disconnect', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const requests = Array.from({ length: 2 }, () =>
    p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000)
  );
  const rejected = requests.map((pending) => assert.rejects(pending.promise, /disconnected/));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(p.source.observerCount(), 2);
  p.source.disconnect('target');
  await Promise.all(rejected);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  assert.equal(p.source.observerCount(), 0);
  for (const mailbox of p.target.queued) mailbox.applicationJob.close();
});

test('recovery reply uses ReplyRouteId independently of OperationId.low', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.operations.register(10000, 'registry', p.placement);
  const envelope = {
    targetMeshName: 'play',
    target: p.placement,
    sourceNodeRid: 'source',
    sourceNodeGeneration: 5n,
    sourceSpotId: 'origin',
    operationKind: 'request',
    operation: { high: 5n, low: 9n },
    replyRouteId: pending.id,
    deadlineUnixMs: BigInt(Date.now() + 10000),
    applicationPayloadFrame: encodeApplicationPayload(p.payload)
  };
  assert.notEqual(envelope.operation.low, envelope.replyRouteId);
  await p.target.runtime.recoverInstanceActivation(envelope, p.route);
  const mailbox = p.target.queued.shift();
  assert.deepEqual(mailbox.stateful.activationRecord.operation, envelope.operation);
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  assert.equal((await pending.promise).terminalResult, RequestResult.Ok);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  assert.equal(p.target.sent.length, 1);
});

test('source-local cold request uses the same logical pending completion without a native reply token', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.target.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  await new Promise((resolve) => setImmediate(resolve));
  const mailbox = p.target.queued.shift();
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  assert.equal((await pending.promise).terminalResult, RequestResult.Ok);
  assert.equal(p.target.runtime.pendingOperationCount, 0);
  assert.equal(p.target.sent.length, 0);
});

test('cold reply failed send is observed once through existing diagnostics', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  const rejected = assert.rejects(pending.promise, /timed out/);
  await new Promise((resolve) => setImmediate(resolve));
  const error = new Error('injected reply send failure');
  let submissions = 0;
  p.target.raw.sendService = async () => {
    submissions++;
    throw error;
  };
  const mailbox = p.target.queued.shift();
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(submissions, 1);
  assert.equal(p.target.reporter.reportedCount, 1);
  assert.ok(flowRecords.some((record) => Object.values(record.attributes).includes(error.message)));
  p.source.runtime.expireOperations(performance.now() + 20000, performance.now() + 1000);
  await rejected;
});
