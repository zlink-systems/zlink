const assert = require('node:assert/strict');
const test = require('node:test');
const telemetry = require('./helpers/telemetry-log-capture');

const {
  ApplicationJobQueue,
  nodeEffectiveProcessorCount,
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

test('local job backlog warns once until every local waiter leaves', async () => {
  telemetry.reset();
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 3n }, () => 1n)
  );
  const held = await Promise.all(
    Array.from({ length: 3 }, () => queue.acquire(undefined, 'remote'))
  );
  const warnings = () =>
    telemetry.records.filter(
      (record) => record.eventId === 'zlink.runtime.host.local_job_backlog_exceeded'
    );
  const pending = [];
  const add = (origin) => {
    const stop = new AbortController();
    const completion = queue.acquire(stop.signal, origin).catch((error) => {
      assert.equal(error.name, 'AbortError');
    });
    pending.push({ stop, completion });
    return stop;
  };
  try {
    for (let i = 0; i < 4; i++) add('remote');
    assert.equal(warnings().length, 0);
    for (let episode = 1; episode <= 2; episode++) {
      const local = Array.from({ length: 3 }, () => add('local'));
      assert.equal(warnings().length, episode - 1);
      const excess = add('local');
      assert.equal(warnings().length, episode);
      assert.equal(warnings().at(-1).localWaiters, 4);
      assert.equal(warnings().at(-1).effectiveMaximum, 3);
      assert.equal(warnings().at(-1).severityText, 'WARN');
      excess.abort();
      const repeated = add('local');
      assert.equal(warnings().length, episode);
      queue.resetMetrics();
      repeated.abort();
      local.forEach((stop) => stop.abort());
    }
  } finally {
    pending.forEach(({ stop }) => stop.abort());
    await Promise.all(pending.map(({ completion }) => completion));
    held.forEach((permit) => permit.releaseAfterInternalProcessing());
  }
});

test('local backlog logger failure preserves the waiting acquisition and episode', async () => {
  const {
    telemetryLogger
  } = require('../../packages/framework/dist/runtime/diagnostics/message-flow');
  const emit = telemetryLogger.emit;
  let attempts = 0;
  telemetryLogger.emit = () => {
    attempts++;
    throw new Error('logger failure');
  };
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
  );
  const held = await queue.acquire(undefined, 'remote');
  const stops = [new AbortController(), new AbortController(), new AbortController()];
  const pending = [];
  try {
    for (const stop of stops) pending.push(queue.acquire(stop.signal, 'local'));
    assert.equal(attempts, 1);
    assert.equal(queue.snapshot().capacityWaiters, 3n);
    held.releaseAfterInternalProcessing();
    const granted = await pending[0];
    assert.equal(granted.origin, 'local');
    granted.releaseAfterInternalProcessing();
    (await pending[1]).releaseAfterInternalProcessing();
    (await pending[2]).releaseAfterInternalProcessing();
    assert.equal(queue.snapshot().capacityWaiters, 0n);
  } finally {
    telemetryLogger.emit = emit;
    stops.forEach((stop) => stop.abort());
    await Promise.allSettled(pending);
    held.releaseAfterInternalProcessing();
  }
});

test('local backlog failure reporter exceptions remain visible', async () => {
  const {
    telemetryLogger
  } = require('../../packages/framework/dist/runtime/diagnostics/message-flow');
  const emit = telemetryLogger.emit;
  const failure = new Error('reporter failure');
  telemetryLogger.emit = () => {
    throw new Error('logger failure');
  };
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n),
    undefined,
    undefined,
    () => {
      throw failure;
    }
  );
  const held = await queue.acquire(undefined, 'remote');
  const stop = new AbortController();
  const first = queue.acquire(stop.signal, 'local');
  try {
    const second = queue.acquire(stop.signal, 'local');
    stop.abort();
    await assert.rejects(second, (error) => error === failure);
  } finally {
    telemetryLogger.emit = emit;
    stop.abort();
    await assert.rejects(first, { name: 'AbortError' });
    held.releaseAfterInternalProcessing();
  }
});

test('ingress record follow-up permits keep their original local origin', async () => {
  telemetry.reset();
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
  );
  const initial = await queue.acquire(undefined, 'local');
  const owner = ApplicationIngressRecordOwner.create(queue, initial, { close() {} });
  const first = owner.takeInitial('application');
  const pending = [owner.acquire('application'), owner.acquire('application')];
  assert.equal(
    telemetry.records.filter(
      (record) => record.eventId === 'zlink.runtime.host.local_job_backlog_exceeded'
    ).length,
    1
  );
  first.releaseBeforeHandler();
  first.close();
  for (const acquisition of pending) {
    const child = await acquisition;
    child.releaseBeforeHandler();
    child.close();
  }
  owner.close();
  assert.equal(queue.snapshot().permitsInUse, 0n);
});

for (const profile of ['constructor', '__proto__']) {
  test(`application job queue rejects inherited profile ${profile} before resolving processors`, () => {
    for (const maxQueuedApplicationJobs of [undefined, 1n]) {
      let processorReads = 0;
      assert.throws(
        () =>
          resolveApplicationJobQueueConfiguration({ profile, maxQueuedApplicationJobs }, () => {
            processorReads++;
            return 1n;
          }),
        {
          name: 'TypeError',
          message: 'applicationJobQueueProfile must be a supported profile.'
        }
      );
      assert.equal(processorReads, 0);
    }
  });
}

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
          configuredPauseThresholdPercent: 80,
          configuredResumeThresholdPercent: 60,
          effectiveProcessorCount: processors,
          effectiveMaxQueuedApplicationJobs: perProcessor * processors,
          pausePermitCount: (perProcessor * processors * 80n + 99n) / 100n,
          resumePermitCount: (perProcessor * processors * 60n) / 100n
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
      configuredPauseThresholdPercent: 80,
      configuredResumeThresholdPercent: 60,
      effectiveProcessorCount: 8n,
      effectiveMaxQueuedApplicationJobs: 17n,
      pausePermitCount: 14n,
      resumePermitCount: 10n
    }
  );

  for (const invalid of [0n, -1n, 2_147_483_648n]) {
    assert.throws(
      () =>
        resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: invalid }, () => 8n),
      /maxQueuedApplicationJobs/u
    );
  }
});

test('effective processor count includes cgroup quota and explicit executor candidates', () => {
  const v2Files = new Map([['/sys/fs/cgroup/cpu.max', '250000 100000']]);
  assert.equal(
    nodeEffectiveProcessorCount(8, 16, (path) => v2Files.get(path)),
    2n
  );

  const fractionalQuota = new Map([['/sys/fs/cgroup/cpu.max', '50000 100000']]);
  assert.equal(
    nodeEffectiveProcessorCount(undefined, 16, (path) => fractionalQuota.get(path)),
    1n
  );

  const v1Files = new Map([
    ['/sys/fs/cgroup/cpu.max', 'max 100000'],
    ['/sys/fs/cgroup/cpu/cpu.cfs_quota_us', '600000'],
    ['/sys/fs/cgroup/cpu/cpu.cfs_period_us', '100000']
  ]);
  assert.equal(
    nodeEffectiveProcessorCount(4, 16, (path) => v1Files.get(path)),
    4n
  );
});

test('application job queue hands a released permit to the oldest live waiter', async () => {
  let now = 0;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 8n),
    () => now
  );

  const first = await queue.acquire(undefined, 'remote');
  first.markApplicationQueued();
  assert.deepEqual(queue.snapshot(), {
    configuredProfile: 'balanced',
    configuredManualMax: 1n,
    configuredPauseThresholdPercent: 80,
    configuredResumeThresholdPercent: 60,
    effectiveProcessorCount: 8n,
    effectiveMaxQueuedApplicationJobs: 1n,
    pausePermitCount: 1n,
    resumePermitCount: 0n,
    reservedSupplyPermits: 0n,
    queuedApplicationJobs: 1n,
    permitsInUse: 1n,
    peakPermitsInUse: 1n,
    capacityWaiters: 0n,
    capacityWaitCount: 0n,
    capacityWaitDurationSeconds: 0,
    pressureState: 'paused',
    currentPauseDurationSeconds: 0,
    runningTransitionCount: 0n,
    pausedTransitionCount: 1n,
    cumulativePauseDurationSeconds: 0,
    flowStateConfigFailureCount: 0n
  });

  const order = [];
  const secondPending = queue.acquire(undefined, 'remote').then((permit) => {
    order.push('second');
    return permit;
  });
  const thirdController = new AbortController();
  const thirdPending = queue.acquire(thirdController.signal, 'remote').then(
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
  assert.equal(queue.snapshot().capacityWaitCount, 2n);
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
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2n }, () => 8n)
  );
  const first = await queue.acquire(undefined, 'remote');
  const second = await queue.acquire(undefined, 'remote');
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

test('capacity waiter metrics stay in the epoch where the wait started', async () => {
  let now = 100;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n),
    () => now
  );
  const active = await queue.acquire(undefined, 'remote');
  const waiter = queue.acquire(undefined, 'remote');
  assert.equal(queue.snapshot().capacityWaitCount, 1n);

  now = 400;
  queue.resetMetrics();
  now = 900;
  active.releaseAfterInternalProcessing();
  const granted = await waiter;

  assert.equal(queue.snapshot().capacityWaitCount, 0n);
  assert.equal(queue.snapshot().capacityWaitDurationSeconds, 0);
  granted.releaseAfterInternalProcessing();
});

test('application job queue validates hysteresis and uses exact ceil/floor permit counts', () => {
  assert.deepEqual(
    resolveApplicationJobQueueConfiguration(
      {
        maxQueuedApplicationJobs: 7n,
        pauseThresholdPercent: 80,
        resumeThresholdPercent: 60
      },
      () => 1n
    ),
    {
      configuredProfile: 'balanced',
      configuredManualMax: 7n,
      configuredPauseThresholdPercent: 80,
      configuredResumeThresholdPercent: 60,
      effectiveProcessorCount: 1n,
      effectiveMaxQueuedApplicationJobs: 7n,
      pausePermitCount: 6n,
      resumePermitCount: 4n
    }
  );

  for (const options of [
    { pauseThresholdPercent: 0 },
    { pauseThresholdPercent: 101 },
    { pauseThresholdPercent: 80.5 },
    { resumeThresholdPercent: -1 },
    { resumeThresholdPercent: 100 },
    { resumeThresholdPercent: 60.5 },
    { pauseThresholdPercent: 60, resumeThresholdPercent: 60 }
  ]) {
    assert.throws(() => resolveApplicationJobQueueConfiguration(options, () => 1n));
  }
});

test('application job queue pressure transitions use permits in use and reset only epoch metrics', async () => {
  let now = 0;
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      {
        maxQueuedApplicationJobs: 5n,
        pauseThresholdPercent: 80,
        resumeThresholdPercent: 40
      },
      () => 1n
    ),
    () => now
  );
  const states = [];
  const unregister = queue.onPressureStateChange((state, sequence) => {
    states.push({ state, sequence });
  });
  const permits = [];
  for (let index = 0; index < 4; index += 1) permits.push(await queue.acquire(undefined, 'remote'));
  assert.equal(queue.snapshot().pressureState, 'paused');
  assert.deepEqual(states, [{ state: 'paused', sequence: 1n }]);

  now = 2_000;
  assert.equal(queue.snapshot().currentPauseDurationSeconds, 2);
  assert.equal(queue.snapshot().cumulativePauseDurationSeconds, 2);
  queue.recordFlowStateConfigFailure();
  queue.resetMetrics();
  assert.equal(queue.snapshot().pressureState, 'paused');
  assert.equal(queue.snapshot().currentPauseDurationSeconds, 2);
  assert.equal(queue.snapshot().cumulativePauseDurationSeconds, 0);
  assert.equal(queue.snapshot().flowStateConfigFailureCount, 0n);

  now = 3_000;
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(queue.snapshot().pressureState, 'paused');
  permits.pop().releaseAfterInternalProcessing();
  assert.equal(queue.snapshot().pressureState, 'running');
  assert.equal(queue.snapshot().cumulativePauseDurationSeconds, 1);
  assert.deepEqual(states, [
    { state: 'paused', sequence: 1n },
    { state: 'running', sequence: 2n }
  ]);
  unregister();
  for (const permit of permits) permit.releaseAfterInternalProcessing();
});

test('one application job queue controller owns channel and raw receive-flow targets', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      {
        maxQueuedApplicationJobs: 2n,
        pauseThresholdPercent: 50,
        resumeThresholdPercent: 0
      },
      () => 1n
    )
  );
  const channel = {};
  const raw = {};
  const states = [];
  assert.equal(
    queue.registerReceiveFlowTarget(channel, (state) => states.push(`channel:${state}`)),
    true
  );
  assert.equal(
    queue.registerReceiveFlowTarget(raw, (state) => states.push(`raw:${state}`)),
    true
  );
  assert.deepEqual(states, ['channel:running', 'raw:running']);

  const permit = await queue.acquire(undefined, 'remote');
  assert.deepEqual(states, ['channel:running', 'raw:running', 'channel:paused', 'raw:paused']);

  queue.unregisterReceiveFlowTarget(channel);
  permit.releaseAfterInternalProcessing();
  assert.deepEqual(states, [
    'channel:running',
    'raw:running',
    'channel:paused',
    'raw:paused',
    'raw:running'
  ]);
  queue.unregisterReceiveFlowTarget(raw);
});

test('receive-flow target may unregister reentrantly during a queue transition', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration(
      {
        maxQueuedApplicationJobs: 1n,
        pauseThresholdPercent: 100,
        resumeThresholdPercent: 0
      },
      () => 1n
    )
  );
  const target = {};
  const states = [];
  queue.registerReceiveFlowTarget(target, (state) => {
    states.push(state);
    if (state === 'paused') queue.unregisterReceiveFlowTarget(target);
  });

  const permit = await queue.acquire(undefined, 'remote');
  permit.releaseAfterInternalProcessing();
  assert.deepEqual(states, ['running', 'paused']);
});

test('initial receive-flow application fences a reentrant public unregister', () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 1n)
  );
  const target = {};
  const states = [];
  assert.throws(
    () =>
      queue.registerReceiveFlowTarget(target, (state) => {
        states.push(state);
        queue.unregisterReceiveFlowTarget(target);
      }),
    /controller is disposed/
  );
  assert.deepEqual(states, ['running']);
  assert.equal(
    queue.registerReceiveFlowTarget(target, (state) => states.push(`replacement:${state}`)),
    true
  );
  queue.unregisterReceiveFlowTarget(target);
  assert.deepEqual(states, ['running', 'replacement:running']);
});

test('application job permit stays queued until the first user callback instruction', async () => {
  const queue = new ApplicationJobQueue(
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 8n)
  );
  const permit = await queue.acquire(undefined, 'remote');
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
    resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 1n }, () => 8n)
  );
  let retainedCloseCount = 0;
  const owner = ApplicationIngressRecordOwner.create(queue, await queue.acquire(undefined, 'remote'), {
    close: () => {
      retainedCloseCount += 1;
    }
  });
  const applicationJob = owner.takeInitial('application');
  owner.close();

  const detached = await runWithApplicationJobPermit(applicationJob, () =>
    detachApplicationJobPermit()
  );
  assert.ok(detached);

  applicationJob.close();
  assert.equal(queue.snapshot().queuedApplicationJobs, 1n);
  assert.equal(queue.snapshot().permitsInUse, 1n);
  assert.equal(retainedCloseCount, 0);

  const nextPending = queue.acquire(undefined, 'remote');
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
