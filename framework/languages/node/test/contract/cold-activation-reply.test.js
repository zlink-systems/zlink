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
const TEST_APPLICATION_JOB_CAPACITY = 1n;
const TEST_NATIVE_REQUEST_SEQUENCE = 1n;

function ingressOwner() {
  const permit = {
    origin: 'remote',
    markApplicationQueued() {},
    detachForHandlerTurn() {},
    releaseBeforeHandler() {},
    releaseAfterInternalProcessing() {}
  };
  return ApplicationIngressRecordOwner.create({ acquire: async () => permit }, permit, {
    close() {}
  });
}

// Core Request supplies the inbound request sequence and opaque reply capability.
function pair() {
  const nodes = new Map();
  const events = [];
  function node(rid, generation) {
    const queued = [];
    const sent = [];
    const requests = [];
    const replyDeliveries = [];
    const activeRequests = new Set();
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
        const permit = await destination.applicationJobQueue.acquire(undefined, 'remote');
        try {
          await destination.receive(received);
          return true;
        } finally {
          permit.releaseAfterInternalProcessing();
        }
      },
      requestService(target, parts, timeoutMs) {
        requests.push({ target, parts, timeoutMs });
        events.push(`request:${rid}:${parts[0][SERVICE_WIRE_COMMAND_OFFSET]}`);
        const destination = nodes.get(target);
        return new Promise((resolve, reject) => {
          let settled = false;
          const finish = (callback, value) => {
            if (settled) return;
            settled = true;
            activeRequests.delete(pendingRequest);
            callback(value);
          };
          const pendingRequest = {
            target,
            fail: (error) => finish(reject, error)
          };
          activeRequests.add(pendingRequest);
          if (!connected || destination === undefined) {
            pendingRequest.fail(new Error(`Core request target '${target}' is disconnected.`));
            return;
          }
          const received = {
            sourceRoutingId: rid,
            parts,
            requestSequence: BigInt(requests.length - 1) + TEST_NATIVE_REQUEST_SEQUENCE,
            reply: (replyParts) => {
              replyDeliveries.push(Promise.resolve());
              finish(
                resolve,
                replyParts.map((part) => Buffer.from(part))
              );
            }
          };
          void destination.applicationJobQueue.acquire(undefined, 'remote').then(
            async (permit) => {
              if (settled) {
                permit.releaseAfterInternalProcessing();
                return;
              }
              try {
                await destination.receive(received);
              } catch (error) {
                pendingRequest.fail(error);
              } finally {
                permit.releaseAfterInternalProcessing();
              }
            },
            (error) => pendingRequest.fail(error)
          );
        });
      },
      replyService(record, parts) {
        if (record.requestSequence === undefined || record.reply === undefined)
          throw new Error('Native reply requires the original request capability.');
        events.push(`reply:${rid}:${parts[0][SERVICE_WIRE_COMMAND_OFFSET]}`);
        record.reply(parts);
      }
    };
    const runtime = new ServiceStatefulRuntime(raw, rid, generation);
    const value = {
      generation,
      runtime,
      queued,
      sent,
      requests,
      replyDeliveries,
      applicationJobQueue,
      raw,
      receive: (record) =>
        ingress({
          command: record.parts[0][SERVICE_WIRE_COMMAND_OFFSET],
          flags: record.parts[0][SERVICE_WIRE_FLAGS_OFFSET],
          applicationJobOwner: ingressOwner(),
          ...record
        }),
      disconnect(peer) {
        connected = false;
        for (const request of [...activeRequests]) {
          if (request.target === peer) {
            request.fail(new Error(`Core request target '${peer}' disconnected.`));
          }
        }
        for (const listener of disconnect) listener(peer);
      },
      observerCount: () => disconnect.size,
      hidePeer() {
        connected = false;
      },
      async occupyAllApplicationPermits() {
        const permits = [];
        const limit = applicationJobQueue.snapshot().effectiveMaxQueuedApplicationJobs;
        while (BigInt(permits.length) < limit) permits.push(await applicationJobQueue.acquire(undefined, 'remote'));
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
      source.disconnect('target');
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
    test(`cold ${recovery ? 'recovered activation survives caller disconnect' : 'request uses the Core reply capability'} (terminal=${terminal})`, async (t) => {
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
      assert.equal(p.source.requests.length, 1);
      const request = wire.decodeStatefulHeader(p.source.requests[0].parts[0]);
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
        const callerFailure = assert.rejects(pending.promise, /disconnected/);
        // A new runtime has no live Core request capability or admitted-operation memory.
        p.restartTarget();
        await callerFailure;
        await p.target.runtime.recoverInstanceActivation(envelope, p.route);
      }
      const mailbox = p.target.queued.shift();
      assert.equal(mailbox.stateful.sourceSpotId, 'origin');
      assert.deepEqual(mailbox.stateful.applicationMetadata, metadataFrame);
      assert.equal(mailbox.stateful.deadlineUnixMs, request.deadlineUnixMs);
      assert.deepEqual(mailbox.parts[1], encodeApplicationPayload(p.payload));
      mailbox.stateful.onHandlerTurnStarted?.();
      if (recovery) {
        assert.equal(mailbox.stateful.reply, undefined);
      } else {
        mailbox.stateful.reply(terminal, 0, terminal === RequestResult.Ok ? p.payload : undefined);
      }
      await mailbox.stateful.onTerminalCompletion();
      mailbox.applicationJob.close();
      assert.equal(p.source.runtime.pendingOperationCount, 0);
      assert.equal(p.target.sent.length, 0);
      if (recovery) {
        assert.equal(mailbox.stateful.activationRecord.replyRouteId, pending.id);
      } else {
        const result = await pending.promise;
        assert.equal(result.terminalResult, terminal);
        if (terminal === RequestResult.Ok)
          assert.deepEqual(result.payload.payload, p.payload.payload);
        assert.equal(p.source.sent.length, 0);
        assert.ok(
          p.events.indexOf('durable:complete') <
            p.events.indexOf(`reply:target:${M6aServiceWireCommand.reply}`)
        );
      }
    });
  }
}

test('cold Core reply completes while source application permits are full', async (t) => {
  const p = pair();
  let releasePermits;
  t.after(async () => {
    releasePermits?.();
    await new Promise((resolve) => setImmediate(resolve));
    p.close();
  });
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  pending.promise.catch(() => {});
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(p.source.requests.length, 1);
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

test('target disconnect fails the cold Core request and suppresses a late reply', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  const rejected = assert.rejects(pending.promise, /disconnected/);
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(p.source.requests.length, 1);
  p.source.disconnect('target');
  await rejected;
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  const mailbox = p.target.queued.shift();
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  assert.equal(p.target.sent.length, 0);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
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

test('cold Core reply with a mismatched logical correlation fails the pending request', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  const rejected = assert.rejects(
    pending.promise,
    (error) => error.kind === ZLinkFrameworkErrorKind.ProtocolError
  );
  await new Promise((resolve) => setImmediate(resolve));
  p.target.raw.replyService = (record) => {
    assert.notEqual(record.requestSequence, undefined);
    assert.equal(typeof record.reply, 'function');
    record.reply([wire.encodeStatefulReply(pending.id + 1n, RequestResult.Ok, 0)]);
  };
  const mailbox = p.target.queued.shift();
  mailbox.stateful.reply(RequestResult.Ok, 0, p.payload);
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  await rejected;
  assert.equal(p.source.runtime.pendingOperationCount, 0);
});

test('cold Core request admission rejection fails the source pending request', async (t) => {
  const p = pair();
  t.after(() => p.close());
  p.source.raw.requestService = async () => {
    throw new Error('Core request admission rejected.');
  };
  const pending = p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000);
  await assert.rejects(pending.promise, /admission rejected/);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  assert.equal(p.source.observerCount(), 0);
  assert.equal(p.target.queued.length, 0);
});

test('cold pending requests use Core disconnect completion without Framework observers', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const requests = Array.from({ length: 2 }, () =>
    p.source.runtime.requestToMissingInstanceSpot(p.placement, p.payload, 10000)
  );
  const rejected = requests.map((pending) => assert.rejects(pending.promise, /disconnected/));
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(p.source.requests.length, 2);
  assert.equal(p.source.observerCount(), 0);
  p.source.disconnect('target');
  await Promise.all(rejected);
  assert.equal(p.source.runtime.pendingOperationCount, 0);
  assert.equal(p.source.observerCount(), 0);
  for (const mailbox of p.target.queued) mailbox.applicationJob.close();
});

test('recovered cold activation preserves its reply correlation without a live Core token', async (t) => {
  const p = pair();
  t.after(() => p.close());
  const replyRouteId = 23n;
  const envelope = {
    targetMeshName: 'play',
    target: p.placement,
    sourceNodeRid: 'source',
    sourceNodeGeneration: 5n,
    sourceSpotId: 'origin',
    operationKind: 'request',
    operation: { high: 5n, low: 9n },
    replyRouteId,
    deadlineUnixMs: BigInt(Date.now() + 10000),
    applicationPayloadFrame: encodeApplicationPayload(p.payload)
  };
  assert.notEqual(envelope.operation.low, replyRouteId);
  await p.target.runtime.recoverInstanceActivation(envelope, p.route);
  const mailbox = p.target.queued.shift();
  assert.deepEqual(mailbox.stateful.activationRecord.operation, envelope.operation);
  assert.equal(mailbox.stateful.activationRecord.replyRouteId, replyRouteId);
  assert.equal(mailbox.stateful.reply, undefined);
  mailbox.stateful.onHandlerTurnStarted?.();
  await mailbox.stateful.onTerminalCompletion();
  mailbox.applicationJob.close();
  assert.equal(p.target.sent.length, 0);
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
