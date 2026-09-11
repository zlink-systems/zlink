'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');

const backend = require('../../packages/framework/dist/runtime/backend');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');

let fixtureSequence = 0;

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((resolvePromise, rejectPromise) => {
    resolve = resolvePromise;
    reject = rejectPromise;
  });
  return { promise, resolve, reject };
}

function createFixture(limit) {
  const suffix = `${process.pid}-${fixtureSequence++}`;
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration(
    limit === undefined ? {} : { maxQueuedApplicationJobs: BigInt(limit) },
    () => limit === undefined ? 2n : 1n
  ));
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName: `application-batch-${suffix}`,
    routingId: `application-batch-node-${suffix}`,
    applicationJobQueue: queue
  });
  const channel = `application-batch-channel-${suffix}`;
  node.setBind(`inproc://application-batch-${suffix}`);
  node.addChannelName(channel);
  node.start();
  return { context, queue, node, channel };
}

async function closeFixture(fixture, pump) {
  await pump?.dispose();
  fixture.node.close();
  await fixture.context.dispose();
}

function observeClaims(node, observeRecords) {
  const createReadyBatch = node.createReadyBatch.bind(node);
  node.createReadyBatch = capacity => {
    const batch = createReadyBatch(capacity);
    const takeClaim = batch.takeClaim.bind(batch);
    batch.takeClaim = index => {
      const claim = takeClaim(index);
      const recvBatch = claim.recvBatch.bind(claim);
      claim.recvBatch = (...args) => {
        const result = recvBatch(...args);
        if (result.records.length > 0) observeRecords(result.records);
        return result;
      };
      return claim;
    };
    return batch;
  };
}

function observeApplicationResidue(node, residues) {
  const drainReady = node.drainReady.bind(node);
  node.drainReady = (...args) => {
    const result = drainReady(...args);
    if (result.records.length > 0) residues.push(result.hasResidue);
    return result;
  };
}

function trackReceivedRecordCleanup(records, observed) {
  for (const record of records) {
    let released = 0;
    const release = record.releaseRetainedIngress;
    record.releaseRetainedIngress = () => {
      released += 1;
      release?.();
    };
    const parts = record.parts.map(part => {
      let closed = 0;
      const close = part.close.bind(part);
      part.close = () => {
        closed += 1;
        close();
      };
      return () => closed;
    });
    observed.push({ released: () => released, parts });
  }
}

test('raw MeshNode drains 130 pre-admitted records as 64, 64, 2 and re-arms from residue', async () => {
  const fixture = createFixture();
  const batches = [];
  const residues = [];
  const received = [];
  const complete = deferred();
  let pump;
  observeClaims(fixture.node, records => batches.push(records.length));
  observeApplicationResidue(fixture.node, residues);
  try {
    for (let index = 0; index < 130; index += 1) {
      await fixture.node.sendToChannel(fixture.channel, Buffer.from(String(index)));
    }
    pump = new backend.ZLinkMeshDispatchPump(fixture.node, {
      applicationJobQueue: fixture.queue,
      dispatch(_owner, record) {
        received.push(Number(record.parts[0].data().toString()));
        if (received.length === 130) complete.resolve();
      },
      reportError: complete.reject
    });
    pump.start();
    await complete.promise;
    await pump.dispose();

    assert.deepEqual(batches, [64, 64, 2]);
    assert.deepEqual(residues, [true, true, false]);
    assert.deepEqual(received, Array.from({ length: 130 }, (_, index) => index));
    assert.equal(new Set(received).size, 130, 'each admitted record dispatches once');
    assert.equal(fixture.queue.snapshot().permitsInUse, 0n);
  } finally {
    await closeFixture(fixture, pump);
  }
});

test('raw MeshNode keeps pre-admitted batch depth within the fixed permit limit', async () => {
  const fixture = createFixture(3);
  const thirdPermit = deferred();
  const firstHandler = deferred();
  const releaseHandlers = deferred();
  const complete = deferred();
  const received = [];
  const acquire = fixture.queue.acquire.bind(fixture.queue);
  let acquired = 0;
  let pump;
  fixture.queue.acquire = async signal => {
    const permit = await acquire(signal);
    acquired += 1;
    if (acquired === 3) thirdPermit.resolve();
    return permit;
  };
  try {
    pump = new backend.ZLinkMeshDispatchPump(fixture.node, {
      applicationJobQueue: fixture.queue,
      async dispatch(_owner, record) {
        if (received.length === 0) firstHandler.resolve();
        await releaseHandlers.promise;
        received.push(Number(record.parts[0].data().toString()));
        if (received.length === 128) complete.resolve();
      },
      reportError: complete.reject
    });
    pump.start();
    const submissions = Array.from(
      { length: 128 },
      (_, index) => fixture.node.sendToChannel(fixture.channel, Buffer.from(String(index)))
    );
    await thirdPermit.promise;
    await firstHandler.promise;

    const saturated = fixture.queue.snapshot();
    assert.equal(saturated.effectiveMaxQueuedApplicationJobs, 3n);
    assert.equal(saturated.permitsInUse, 3n);
    assert.equal(saturated.peakPermitsInUse, 3n);

    releaseHandlers.resolve();
    await Promise.all(submissions);
    await complete.promise;
    await pump.dispose();
    assert.deepEqual(received, Array.from({ length: 128 }, (_, index) => index));
    const drained = fixture.queue.snapshot();
    assert.equal(drained.peakPermitsInUse, 3n);
    assert.equal(drained.permitsInUse, 0n);
    assert.equal(drained.queuedApplicationJobs, 0n);
  } finally {
    releaseHandlers.resolve();
    await closeFixture(fixture, pump);
  }
});

test('part-cap partial receive re-arms from claim release when drainReady reported no residue', async () => {
  const fixture = createFixture();
  const batches = [];
  const residues = [];
  const received = [];
  const payloads = [
    [Buffer.from([0, 1]), Buffer.from([2, 3])],
    [Buffer.from([4, 5]), Buffer.from([6, 7])]
  ];
  const complete = deferred();
  let pump;
  observeClaims(fixture.node, records => batches.push(records.length));
  observeApplicationResidue(fixture.node, residues);
  try {
    for (const payload of payloads) {
      await fixture.node.sendToChannel(fixture.channel, payload);
    }
    pump = new backend.ZLinkMeshDispatchPump(fixture.node, {
      applicationJobQueue: fixture.queue,
      partCapacity: 3,
      dispatch(_owner, record) {
        received.push(record.parts.map(part => Buffer.from(part.data())));
        if (received.length === payloads.length) complete.resolve();
      },
      reportError: complete.reject
    });
    pump.start();
    await complete.promise;
    await pump.dispose();

    assert.deepEqual(batches, [1, 1]);
    assert.deepEqual(residues, [false, false]);
    assert.deepEqual(received, payloads, 'claim release preserves original application bytes');
    assert.equal(fixture.queue.snapshot().permitsInUse, 0n);
  } finally {
    await closeFixture(fixture, pump);
  }
});

test('dispatch failure returns every pre-admitted lease and closes every batch part once', async () => {
  const fixture = createFixture();
  const observed = [];
  const failure = new Error('dispatch failed');
  const reported = deferred();
  let dispatches = 0;
  let pump;
  observeClaims(fixture.node, records => trackReceivedRecordCleanup(records, observed));
  try {
    for (let index = 0; index < 64; index += 1) {
      await fixture.node.sendToChannel(fixture.channel, Buffer.from(String(index)));
    }
    pump = new backend.ZLinkMeshDispatchPump(fixture.node, {
      applicationJobQueue: fixture.queue,
      dispatch() {
        dispatches += 1;
        throw failure;
      },
      reportError: reported.resolve
    });
    pump.start();
    assert.equal(await reported.promise, failure);

    assert.equal(dispatches, 1);
    assert.equal(observed.length, 64);
    for (const record of observed) {
      assert.equal(record.released(), 1);
      for (const closed of record.parts) assert.equal(closed(), 1);
    }
    const snapshot = fixture.queue.snapshot();
    assert.equal(snapshot.permitsInUse, 0n);
    assert.equal(snapshot.queuedApplicationJobs, 0n);
  } finally {
    await closeFixture(fixture, pump);
  }
});

test('dispose during a pre-admitted batch returns unstarted leases and closes their parts once', async () => {
  const fixture = createFixture();
  const observed = [];
  const firstDispatch = deferred();
  const releaseFirst = deferred();
  let dispatches = 0;
  let pump;
  observeClaims(fixture.node, records => trackReceivedRecordCleanup(records, observed));
  try {
    for (let index = 0; index < 64; index += 1) {
      await fixture.node.sendToChannel(fixture.channel, Buffer.from(String(index)));
    }
    pump = new backend.ZLinkMeshDispatchPump(fixture.node, {
      applicationJobQueue: fixture.queue,
      async dispatch() {
        dispatches += 1;
        firstDispatch.resolve();
        await releaseFirst.promise;
      }
    });
    pump.start();
    await firstDispatch.promise;

    const disposing = pump.dispose();
    releaseFirst.resolve();
    await disposing;

    assert.equal(dispatches, 1);
    assert.equal(observed.length, 64);
    for (const record of observed) {
      assert.equal(record.released(), 1);
      for (const closed of record.parts) assert.equal(closed(), 1);
    }
    const snapshot = fixture.queue.snapshot();
    assert.equal(snapshot.permitsInUse, 0n);
    assert.equal(snapshot.queuedApplicationJobs, 0n);
  } finally {
    releaseFirst.resolve();
    await closeFixture(fixture, pump);
  }
});
