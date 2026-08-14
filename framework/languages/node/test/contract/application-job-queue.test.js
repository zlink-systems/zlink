const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  detachApplicationJobPermit,
  releaseApplicationJobPermitBeforeHandler,
  runWithApplicationJobPermit
} = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const {
  ApplicationIngressRecordOwner
} = require('../../packages/framework/dist/runtime/application-jobs/application-ingress-record-owner');

test('application job queue resolves the exact profile matrix and manual override', () => {
  const expected = new Map([
    ['compact', 32n],
    ['low_latency', 64n],
    ['balanced', 128n],
    ['throughput', 256n]
  ]);

  for (const processors of [4n, 8n, 16n]) {
    for (const [profile, perProcessor] of expected) {
      assert.deepEqual(
        resolveApplicationJobQueueConfiguration({ profile }, () => processors),
        {
          configuredProfile: profile,
          configuredManualMax: undefined,
          effectiveProcessorCount: processors,
          effectiveMaxQueuedApplicationJobs: perProcessor * processors
        }
      );
    }
  }

  assert.deepEqual(
    resolveApplicationJobQueueConfiguration(
      { profile: 'throughput', maxQueuedApplicationJobs: 17n },
      () => 8n
    ),
    {
      configuredProfile: 'throughput',
      configuredManualMax: 17n,
      effectiveProcessorCount: 8n,
      effectiveMaxQueuedApplicationJobs: 17n
    }
  );

  for (const invalid of [0n, -1n, 2_147_483_648n]) {
    assert.throws(
      () => resolveApplicationJobQueueConfiguration(
        { maxQueuedApplicationJobs: invalid },
        () => 8n
      ),
      /maxQueuedApplicationJobs/u
    );
  }
});

test('application job queue hands a released permit to the oldest live waiter', async () => {
  let now = 0;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 1n },
      () => 8n
    ),
    () => now
  );

  const first = await queue.acquire();
  first.markApplicationQueued();
  assert.deepEqual(queue.snapshot(), {
    configuredProfile: 'balanced',
    configuredManualMax: 1n,
    effectiveProcessorCount: 8n,
    effectiveMaxQueuedApplicationJobs: 1n,
    reservedSupplyPermits: 0n,
    queuedApplicationJobs: 1n,
    permitsInUse: 1n,
    peakPermitsInUse: 1n,
    capacityWaiters: 0n,
    capacityWaitCount: 0n,
    capacityWaitDurationSeconds: 0
  });

  const order = [];
  const secondPending = queue.acquire().then((permit) => {
    order.push('second');
    return permit;
  });
  const thirdController = new AbortController();
  const thirdPending = queue.acquire(thirdController.signal).then(
    () => order.push('third'),
    () => order.push('third-cancelled')
  );
  await Promise.resolve();
  assert.equal(queue.snapshot().capacityWaiters, 2n);

  now = 250;
  first.releaseBeforeHandler();
  const second = await secondPending;
  assert.deepEqual(order, ['second']);
  assert.equal(queue.snapshot().reservedSupplyPermits, 1n);
  assert.equal(queue.snapshot().permitsInUse, 1n);
  assert.equal(queue.snapshot().capacityWaitCount, 1n);
  assert.equal(queue.snapshot().capacityWaitDurationSeconds, 0.25);

  thirdController.abort(new Error('cancel oldest waiter'));
  await thirdPending;
  assert.deepEqual(order, ['second', 'third-cancelled']);
  assert.equal(queue.snapshot().capacityWaiters, 0n);

  second.releaseAfterInternalProcessing();
  assert.equal(queue.snapshot().permitsInUse, 0n);
  assert.equal(queue.snapshot().reservedSupplyPermits, 0n);
  assert.equal(queue.snapshot().queuedApplicationJobs, 0n);
});

test('application job queue reset keeps gauges and rebases the peak', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 2n },
      () => 8n
    )
  );
  const first = await queue.acquire();
  const second = await queue.acquire();
  first.markApplicationQueued();
  second.markApplicationQueued();

  queue.resetMetrics();
  const reset = queue.snapshot();
  assert.equal(reset.queuedApplicationJobs, 2n);
  assert.equal(reset.permitsInUse, 2n);
  assert.equal(reset.peakPermitsInUse, 2n);
  assert.equal(reset.capacityWaitCount, 0n);
  assert.equal(reset.capacityWaitDurationSeconds, 0);

  first.releaseBeforeHandler();
  second.releaseBeforeHandler();
});

test('application job permit stays queued until the first user callback instruction', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 1n },
      () => 8n
    )
  );
  const permit = await queue.acquire();
  permit.markApplicationQueued();

  await runWithApplicationJobPermit(permit, async () => {
    assert.equal(queue.snapshot().queuedApplicationJobs, 1n);
    releaseApplicationJobPermitBeforeHandler();
    assert.equal(queue.snapshot().queuedApplicationJobs, 0n);
  });

  assert.equal(queue.snapshot().permitsInUse, 0n);
});

test('detached exact-target turn retains its ingress permit until callback start', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      { maxQueuedApplicationJobs: 1n },
      () => 8n
    )
  );
  let retainedCloseCount = 0;
  const owner = ApplicationIngressRecordOwner.create(
    queue,
    await queue.acquire(),
    { close: () => { retainedCloseCount += 1; } }
  );
  const applicationJob = owner.takeInitial('application');
  owner.close();

  const detached = await runWithApplicationJobPermit(
    applicationJob,
    () => detachApplicationJobPermit()
  );
  assert.ok(detached);

  applicationJob.close();
  assert.equal(queue.snapshot().queuedApplicationJobs, 1n);
  assert.equal(queue.snapshot().permitsInUse, 1n);
  assert.equal(retainedCloseCount, 0);

  const nextPending = queue.acquire();
  await Promise.resolve();
  assert.equal(queue.snapshot().capacityWaiters, 1n);

  detached.releaseBeforeHandler();
  const next = await nextPending;
  assert.equal(retainedCloseCount, 0);
  assert.equal(queue.snapshot().reservedSupplyPermits, 1n);

  detached.releaseAfterInternalProcessing();
  assert.equal(retainedCloseCount, 1);

  next.releaseAfterInternalProcessing();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});
