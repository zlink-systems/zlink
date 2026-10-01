const assert = require('node:assert/strict');
const test = require('node:test');
const { SubmitResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
const {
  classifySubmitResult,
  requireOneWayCompletion
} = require('../../packages/framework/dist/runtime/messaging/submission-result');
const { ZLinkFrameworkErrorKind } = require('../../packages/framework/dist/contracts');
const {
  zlinkRuntimeDefaultLocationOptions
} = require('../../packages/framework/dist/contracts/Locations/Options');
const {
  putNewRelocationBlob
} = require('../../packages/framework/dist/runtime/locations/relocation-blob');

test('location queries default to a page of 100', () => {
  assert.equal(zlinkRuntimeDefaultLocationOptions.listPageSize, 100);
});

test('generated channel weight codec accepts glossary bounds and rejects values outside them', () => {
  const { encodeChannelEntry, decodeChannelEntry } = require('../../packages/framework/dist/runtime/protocol/service_wire_codec.generated');
  for (const weight of [0, 10_000]) {
    const entry = { channelName: 'orders', weight };
    assert.deepEqual(decodeChannelEntry(encodeChannelEntry(entry, {}), {}), entry);
  }
  for (const weight of [-1, 10_001]) {
    assert.throws(() => encodeChannelEntry({ channelName: 'orders', weight }, {}), /weight constraint/u);
  }
  const encoded = Buffer.from(encodeChannelEntry({ channelName: 'orders', weight: 0 }, {}));
  encoded.writeUInt32BE(10_001, encoded.length - 4);
  assert.throws(() => decodeChannelEntry(encoded, {}), /weight constraint/u);
});

test('one-way NOT_ADMITTED completes with Rejected', () => {
  assert.throws(
    () => requireOneWayCompletion(classifySubmitResult(SubmitResult.NotAdmitted, 'Actor send'), 'Actor send'),
    (error) => error.kind === ZLinkFrameworkErrorKind.Rejected
  );
});

test('uncertain relocation put confirms the same reference and bytes using operation cancellation', async () => {
  const payload = Uint8Array.of(1, 2, 3);
  const signal = new AbortController().signal;
  const expiresAt = new Date(1000);
  const storeNow = new Date(0);
  let writtenReference;
  const store = {
    async put(reference, bytes, retentionMs, operationSignal) {
      writtenReference = reference;
      assert.equal(bytes, payload);
      assert.equal(operationSignal, signal);
      throw new Error('Put response lost');
    },
    async read(reference, operationSignal) {
      assert.equal(reference, writtenReference);
      assert.equal(operationSignal, signal);
      return { kind: 'found', bytes: payload.slice(), expiresAt, storeNow };
    }
  };
  assert.deepEqual(await putNewRelocationBlob(store, payload, 1000, signal), {
    reference: writtenReference,
    expiresAt,
    storeNow
  });
});

test('relocation reconciliation stores missing bytes at the same reference and conflicting bytes at a new reference', async () => {
  const failure = new Error('Put response lost');
  for (const read of [
    async () => ({ kind: 'missing', storeNow: new Date(0) }),
    async () => ({ kind: 'found', bytes: Uint8Array.of(4), expiresAt: new Date(1000), storeNow: new Date(0) })
  ]) {
    const references = [];
    const result = await putNewRelocationBlob({
      async put(reference) {
        references.push(reference);
        if (references.length === 1) throw failure;
        return { kind: 'stored', expiresAt: new Date(1000), storeNow: new Date(0) };
      }, read
    }, Uint8Array.of(1), 1000);
    const confirmation = await read();
    assert.equal(references.length, 2);
    assert.equal(references[0] === references[1], confirmation.kind === 'missing');
    assert.equal(result.reference, references[1]);
  }
});

test('relocation conflicts have no attempt cap and end at the operation deadline', async () => {
  const references = [];
  const result = await putNewRelocationBlob({
    async put(reference) {
      references.push(reference.value);
      return references.length <= 5
        ? { kind: 'conflict', storeNow: new Date(0) }
        : { kind: 'stored', expiresAt: new Date(1000), storeNow: new Date(0) };
    }
  }, Uint8Array.of(1), 1000);
  assert.equal(references.length, 6);
  assert.equal(new Set(references).size, 6);
  assert.equal(result.reference.value, references[5]);
  const controller = new AbortController();
  const deadline = new Error('Operation deadline exceeded');
  await assert.rejects(putNewRelocationBlob({
    async put() {
      controller.abort(deadline);
      return { kind: 'conflict', storeNow: new Date(0) };
    }
  }, Uint8Array.of(1), 1000, controller.signal), (error) => error === deadline);
});

test('relocation reconciliation exposes provider read errors and operation cancellation', async () => {
  const readFailure = new Error('Read response lost');
  const putFailure = new Error('Put response lost');
  await assert.rejects(putNewRelocationBlob({
    async put() { throw putFailure; },
    async read() { throw readFailure; }
  }, Uint8Array.of(1), 1000), (error) => error === readFailure);
  const controller = new AbortController();
  const cancelled = new Error('Operation cancelled during put');
  await assert.rejects(putNewRelocationBlob({
    async put() { controller.abort(cancelled); throw putFailure; },
    async read() { assert.fail('Read must not start after operation cancellation'); }
  }, Uint8Array.of(1), 1000, controller.signal), (error) => error === cancelled);
});

test('relocation does not start provider I/O when operation cancellation is already requested', async () => {
  const controller = new AbortController();
  const reason = new Error('Operation cancelled');
  controller.abort(reason);
  await assert.rejects(putNewRelocationBlob({ async put() { assert.fail('Provider must not be called'); } }, Uint8Array.of(1), 1000, controller.signal),
    (error) => error === reason);
});

for (const pending of ['put', 'read']) {
  test(`operation cancellation ends a pending relocation ${pending} waiter`, { timeout: 1000 }, async (context) => {
    const controller = new AbortController();
    let release;
    const held = new Promise((resolve) => { release = resolve; });
    context.after(() => release(pending === 'put'
      ? { kind: 'stored', expiresAt: new Date(1000), storeNow: new Date(0) }
      : { kind: 'missing', storeNow: new Date(0) }));
    const store = {
      async put() {
        if (pending === 'read') throw new Error('Put response lost');
        queueMicrotask(() => controller.abort());
        return held;
      },
      async read() {
        queueMicrotask(() => controller.abort());
        return held;
      }
    };
    await assert.rejects(putNewRelocationBlob(store, Uint8Array.of(1), 1000, controller.signal),
      (error) => error.name === 'AbortError');
  });
}

test('Instance activation propagates the operation signal to payload put and confirmation', async () => {
  const { ZLinkInstanceActivationAuthority } = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
  const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const signal = new AbortController().signal;
  const confirmed = new Error('Confirmation observed');
  const authority = new ZLinkInstanceActivationAuthority({
    meshName: 'gap-loc',
    store: {},
    owner: () => ({ ownerId: 'owner', leaseGeneration: 1n }),
    relocationStore: {
      async put(_reference, _bytes, _retention, operationSignal) {
        assert.equal(operationSignal, signal);
        return { kind: 'stored', expiresAt: new Date(1000), storeNow: new Date(0) };
      },
      async read(_reference, operationSignal) {
        assert.equal(operationSignal, signal);
        throw confirmed;
      }
    }
  });
  await assert.rejects(authority.reserve({
    target: { targetSpotId: 'spot', stableType: 'Tenant', targetNodeRid: 'node', targetNodeGeneration: 1n, descriptorVersion: 'v1' },
    sourceNodeRid: 'source', sourceNodeGeneration: 1n,
    operationKind: 'send', operation: { high: 1n, low: 2n },
    deadlineUnixMs: BigInt(Date.now() + 1000),
    applicationPayloadFrame: encodeApplicationPayload({ packetName: 'TenantRequest', contentType: 'application/octet-stream', payload: Uint8Array.of(1) })
  }, signal), (error) => error === confirmed);
});

test('operation deadline cancellation completes with DeadlineExceeded instead of InternalFailure', async (context) => {
  const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
  const { M6bServiceWireCommand, encodeUserSpotCreateHeader, decodeStatefulReply } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { ApplicationIngressRecordOwner } = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  context.mock.timers.enable({ apis: ['setTimeout'] });
  let ingress;
  let handlerStarted;
  const started = new Promise((resolve) => { handlerStarted = resolve; });
  let replied;
  const reply = new Promise((resolve) => { replied = resolve; });
  const permit = {
    markApplicationQueued() {}, detachForHandlerTurn() {},
    releaseBeforeHandler() {}, releaseAfterInternalProcessing() {}
  };
  const runtime = new ServiceStatefulRuntime({
    topology: { peer: () => ({ descriptor: { lifecycleGeneration: 5n } }) },
    observePeerConnectionIntentRemoved: () => () => {},
    setServiceIngress(handler) { ingress = handler; },
    replyService(_record, parts) { replied(parts); }
  }, 'target', 7n);
  try {
    runtime.registerUserSpotOperationHandler({
      async create(_record, signal) {
        return await new Promise((_resolve, reject) => {
          signal.addEventListener('abort', () => reject(signal.reason), { once: true });
          handlerStarted();
        });
      },
      async close() { assert.fail('Close must not be called'); }
    });
    await ingress({
      command: M6bServiceWireCommand.userSpotCreate, flags: 0,
      sourceRoutingId: 'source', requestSequence: 1n,
      applicationJobOwner: ApplicationIngressRecordOwner.create({ acquire: async () => permit }, permit, { close() {} }),
      parts: [encodeUserSpotCreateHeader({
        operation: { high: 1n, low: 2n }, correlation: 3n,
        sourceNodeRid: 'source', sourceNodeGeneration: 5n,
        spotId: 'gap-loc-spot', stableType: 'Room',
        reservation: {
          reservationId: 'reservation', expectedStoreVersion: 'version',
          objectGeneration: 1n, authorityOwnerGeneration: 1n,
          targetNodeRid: 'target', targetNodeGeneration: 7n,
          targetOwnerId: 'owner', targetOwnerLeaseGeneration: 1n,
          pendingCapacityDelta: 1
        },
        deadlineUnixMs: BigInt(Date.now() + 1000)
      })]
    });
    await started;
    context.mock.timers.tick(1000);
    assert.equal(decodeStatefulReply((await reply)[0], 3n, 'userSpotCreate').terminalResult, RequestResult.TimedOut);
  } finally {
    runtime.close();
  }
});

test('Instance activation does not start payload put when its deadline expires during authority read', async (context) => {
  const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
  const { ZLinkInstanceActivationAuthority } = require('../../packages/framework/dist/runtime/host/instance-activation-authority');
  const { M6bServiceWireCommand, encodeInstanceSpotActivationHeader } = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
  const { encodeApplicationPayload } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  const { ApplicationIngressRecordOwner } = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');
  const now = Date.now();
  const deadline = BigInt(now + 1000);
  context.mock.timers.enable({ apis: ['Date', 'setTimeout'], now });
  let ingress;
  let reserveStarted;
  const reserveReached = new Promise((resolve) => { reserveStarted = resolve; });
  let puts = 0;
  const providerAuthority = new ZLinkInstanceActivationAuthority({
    meshName: 'gap-loc', store: {},
    owner: () => ({ ownerId: 'owner', leaseGeneration: 1n }),
    relocationStore: {
      async put() { puts++; throw new Error('Unexpected expired-operation put'); },
      async read() { return { kind: 'missing', storeNow: new Date() }; }
    }
  });
  const permit = {
    markApplicationQueued() {}, detachForHandlerTurn() {},
    releaseBeforeHandler() {}, releaseAfterInternalProcessing() {}
  };
  const runtime = new ServiceStatefulRuntime({
    topology: { peer: () => ({ descriptor: { lifecycleGeneration: 7n } }) },
    observePeerConnectionIntentRemoved: () => () => {},
    setServiceIngress(handler) { ingress = handler; }
  }, 'target', 3n);
  try {
    runtime.registerAsyncInstanceActivationAuthority({
      async read() {
        context.mock.timers.setTime(Number(deadline) + 1);
        return { kind: 'missing' };
      },
      reserve(activation, signal) {
        reserveStarted(signal);
        return providerAuthority.reserve(activation, signal);
      }
    });
    await ingress({
      command: M6bServiceWireCommand.instanceSpot, flags: 0,
      sourceRoutingId: 'source',
      applicationJobOwner: ApplicationIngressRecordOwner.create({ acquire: async () => permit }, permit, { close() {} }),
      parts: [encodeInstanceSpotActivationHeader({
        targetNodeRid: 'target', targetNodeGeneration: 3n,
        targetSpotId: 'expired-gap-loc', stableType: 'Tenant', descriptorVersion: 'v1'
      }, 7n, 'source', undefined, 'send', { high: 1n, low: 2n }, deadline),
      encodeApplicationPayload({ packetName: 'TenantRequest', contentType: 'application/octet-stream', payload: Uint8Array.of(1) })]
    });
    const signal = await reserveReached;
    assert.equal(puts, 0);
    assert.equal(signal.aborted, true);
  } finally {
    runtime.close();
  }
});

for (const retryResult of ['conflict', 'missing']) {
  test(`Relocation Store immediate ${retryResult} responses allow the operation deadline to terminate`, () => {
    const { execFileSync } = require('node:child_process');
    const repositoryPath = require.resolve('../../packages/framework/dist/runtime/locations/relocation-blob');
    const operationDeadlineMs = 20;
    const childWatchdogMs = 1000;
    const source = `
      const assert = require('node:assert/strict');
      const { putNewRelocationBlob } = require(${JSON.stringify(repositoryPath)});
      const signal = AbortSignal.timeout(${operationDeadlineMs});
      const references = [];
      const store = {
        async put(reference) {
          references.push(reference.value);
          if (${JSON.stringify(retryResult)} === 'missing') throw new Error('Put response lost');
          return { kind: 'conflict', storeNow: new Date(0) };
        },
        async read() { return { kind: 'missing', storeNow: new Date(0) }; }
      };
      putNewRelocationBlob(store, Uint8Array.of(1), 1000, signal).then(
        () => { throw new Error('Unexpected Put success'); },
        (error) => {
          assert.equal(signal.aborted, true);
          assert.equal(error, signal.reason);
          assert.ok(references.length > 0);
          if (${JSON.stringify(retryResult)} === 'missing') assert.equal(new Set(references).size, 1);
          console.log('operation-deadline=terminated');
        }
      );
    `;
    const output = execFileSync(process.execPath, ['-e', source], {
      timeout: childWatchdogMs,
      encoding: 'utf8'
    });
    assert.match(output, /operation-deadline=terminated/u);
  });
}
