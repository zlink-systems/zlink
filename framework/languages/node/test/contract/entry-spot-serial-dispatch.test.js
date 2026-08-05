const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const zlink = require('@zlink-systems/zlink');
const framework = require('../../packages/framework/dist/internal');

class PlayerActor {
  constructor(context) {
    this.actorId = context.actorId;
    this.context = context;
  }
}

class PlayerFactory {
  async create(context, _signal) {
    return new PlayerActor(context);
  }
}

async function createEntryFixture(entrySpotType, packetHandlers = [], options = {}) {
  const manager = new framework.DefaultZLinkActorManager({
    actorFactories: new Map([['player', PlayerFactory]]),
    actorMeshNameProvider: () => 'test.mesh'
  });
  const activation = new framework.ZLinkEntrySpotActivation({
    entrySpotType,
    nativeSpot: { routingId: 'entry-test', async dispose() {} },
    nodeRid: 'node-test',
    spotNodeName: 'node-test',
    ...options
  });
  await activation.create();
  const registry = new framework.ZLinkSpotActorHandlerRegistryRuntime();
  for (const descriptor of packetHandlers) {
    registry.addPacket(descriptor);
  }
  const dispatcher = new framework.ZLinkSpotActorDispatcher({
    registry,
    spot: activation.entrySpot
  });
  const mailboxes = new framework.ZLinkActorDispatchMailboxSet();
  const router = {
    submit(actorId, operation) {
      return mailboxes.submit(actorId, () => {
        const state = manager.getState(actorId);
        if (state?.actor === undefined) throw new Error(`Actor '${actorId}' is not created.`);
        return operation({ actor: state.actor, spotId: state.spotId });
      });
    }
  };
  return { manager, activation, registry, dispatcher, router };
}

function overlapTracker(events) {
  let active = 0;
  let overlapped = false;
  return {
    async run(name, body) {
      active += 1;
      overlapped = overlapped || active > 1;
      events.push(`${name}:start`);
      try {
        await body?.();
      } finally {
        events.push(`${name}:end`);
        active -= 1;
      }
    },
    get overlapped() {
      return overlapped;
    }
  };
}

function delay(durationMs) {
  return new Promise((resolve) => setTimeout(resolve, durationMs));
}

function initializeLifecycleIdentity(manager, actor) {
  manager.getState(actor.actorId).setNativeActorRef({
    nodeRid: zlink.RoutingId.from('node-test'),
    actorId: actor.actorId,
    generation: 1n
  });
}

test('entry spot lifecycle callbacks from mixed setImmediate/queueMicrotask backend callbacks keep enqueue order without overlap', async () => {
  const events = [];
  const tracker = overlapTracker(events);
  class AdmissionEntrySpot {
    onCreateActor(actor) {
      return tracker.run(`create:${actor.actorId}`, () => delay(2));
    }
  }
  const fixture = await createEntryFixture(AdmissionEntrySpot);
  const alice = await fixture.manager.getOrCreateActor('alice', 'player');
  const bob = await fixture.manager.getOrCreateActor('bob', 'player');
  initializeLifecycleIdentity(fixture.manager, alice);
  initializeLifecycleIdentity(fixture.manager, bob);

  // Native/binding callbacks arrive from mixed macrotask/microtask contexts;
  // each enqueues into the Entry Spot serial executor instead of invoking
  // user code directly.
  const submissions = [];
  const enqueueOrder = [];
  const fromBackend = (schedule, name, dispatch) => new Promise((resolve) => {
    schedule(() => {
      enqueueOrder.push(name);
      submissions.push(dispatch());
      resolve();
    });
  });
  await fromBackend(queueMicrotask, 'create:alice', () =>
    fixture.activation.notifyCreateActor(alice, framework.ZLinkMessage.from({})));
  await fromBackend(setImmediate, 'create:bob', () =>
    fixture.activation.notifyCreateActor(bob, framework.ZLinkMessage.from({})));
  await Promise.all(submissions);

  assert.equal(tracker.overlapped, false);
  const startOrder = events
    .filter((event) => event.endsWith(':start'))
    .map((event) => event.slice(0, -':start'.length));
  assert.deepEqual(startOrder, enqueueOrder);
  for (let index = 0; index < events.length; index += 2) {
    assert.equal(events[index].endsWith(':start'), true);
    assert.equal(events[index + 1], events[index].replace(':start', ':end'));
  }
  assert.equal(alice instanceof PlayerActor, true);
});

test('entry spot does not start the next callback before the previous handler promise settles', async () => {
  const events = [];
  let releaseFirst;
  const firstGate = new Promise((resolve) => {
    releaseFirst = resolve;
  });
  class GatedEntrySpot {
    async onCreateActor(actor) {
      events.push(`create:${actor.actorId}:start`);
      await firstGate;
      events.push(`create:${actor.actorId}:settled`);
    }
  }
  const fixture = await createEntryFixture(GatedEntrySpot);
  const alice = await fixture.manager.getOrCreateActor('alice', 'player');
  const bob = await fixture.manager.getOrCreateActor('bob', 'player');
  initializeLifecycleIdentity(fixture.manager, alice);
  initializeLifecycleIdentity(fixture.manager, bob);

  const first = fixture.activation.notifyCreateActor(alice, framework.ZLinkMessage.from({}));
  const second = fixture.activation.notifyCreateActor(bob, framework.ZLinkMessage.from({}));
  await delay(10);
  assert.deepEqual(events, ['create:alice:start']);

  releaseFirst();
  await Promise.all([first, second]);
  assert.deepEqual(events, ['create:alice:start', 'create:alice:settled', 'create:bob:start', 'create:bob:settled']);
});

test('entry spot actor packets use actor mailboxes without entry-wide serial dispatch', async () => {
  const events = [];
  class EntrySpot {}
  class SlowPacketHandler {
    async handle(_spot, actor, _context, message) {
      events.push(`${actor.actorId}:${message}:start`);
      if (message === 'block') {
        await delay(40);
      }
      events.push(`${actor.actorId}:${message}:end`);
    }
  }
  const fixture = await createEntryFixture(EntrySpot, [{
    kind: framework.ZLinkActorPacketKind.Send,
    packetName: 'packet',
    actorType: PlayerActor,
    handlerType: SlowPacketHandler
  }]);
  await fixture.manager.getOrCreateActor('actor-a', 'player');
  await fixture.manager.getOrCreateActor('actor-b', 'player');

  const firstA = fixture.router.submit('actor-a', (snapshot) =>
    fixture.dispatcher.dispatchSend(snapshot.actor, 'packet', 'block'));
  await delay(5);
  const firstB = fixture.router.submit('actor-b', (snapshot) =>
    fixture.dispatcher.dispatchSend(snapshot.actor, 'packet', 'fast'));
  const secondA = fixture.router.submit('actor-a', (snapshot) =>
    fixture.dispatcher.dispatchSend(snapshot.actor, 'packet', 'second'));
  await Promise.all([firstA, firstB, secondA]);

  assert.equal(events.indexOf('actor-b:fast:start') < events.indexOf('actor-a:block:end'), true);
  assert.equal(events.indexOf('actor-a:second:start') > events.indexOf('actor-a:block:end'), true);
});

test('entry spot actor handler submit surfaces outbound failure immediately', async () => {
  let observed;
  let outbound;
  const channelClient = {
    requestToChannel() {
      return {
        packetName() { return this; },
        timeout() { return this; },
        submit() {
          throw new Error('yield must fail before starting the outbound request');
        }
      };
    },
    sendToChannel() { throw new Error('not used'); }
  };
  class EntrySpot {}
  class YieldPacketHandler {
    async handle() {
      try {
        await outbound.requestToChannel('delay', { value: 'ping' }).submit();
      } catch (error) {
        observed = error;
        return;
      }
      throw new Error('yield unexpectedly completed inside an Entry Spot actor handler');
    }
  }
  const fixture = await createEntryFixture(EntrySpot, [{
    kind: framework.ZLinkActorPacketKind.Send,
    packetName: 'yield',
    actorType: PlayerActor,
    handlerType: YieldPacketHandler
  }], { channelClient });
  outbound = fixture.activation.context.outbound;
  await fixture.manager.getOrCreateActor('actor-yield', 'player');

  await fixture.router.submit('actor-yield', (snapshot) =>
    fixture.dispatcher.dispatchSend(snapshot.actor, 'yield', 'run'));

  assert.equal(observed instanceof Error, true);
  assert.match(observed.message, /yield must fail before starting/);
});

test('joined user spot actors keep per-actor mailbox dispatch off the entry line', async () => {
  class IdleEntrySpot {}
  const fixture = await createEntryFixture(IdleEntrySpot);
  await fixture.manager.getOrCreateActor('alice', 'player');
  fixture.manager.getState('alice').setJoinedSpot('stage-1', { context: { spotId: 'stage-1' } });

  let releaseEntry;
  const entryGate = new Promise((resolve) => {
    releaseEntry = resolve;
  });
  const entryBusy = fixture.activation.serialExecutor.execute(() => entryGate);

  // A user-Spot-joined actor must proceed while the Entry Spot line is busy.
  const joined = await fixture.router.submit('alice', (snapshot) => `spot:${snapshot.spotId}`);
  assert.equal(joined, 'spot:stage-1');
  releaseEntry();
  await entryBusy;
});

test('detached request continuation re-enters the entry spot serial line', async () => {
  const events = [];
  let resolveReply;
  const channelClient = {
    requestToChannel() {
      return {
        packetName() { return this; },
        timeout() { return this; },
        submit() {
          events.push('request:submitted');
          return new Promise((resolve) => {
            resolveReply = resolve;
          });
        }
      };
    },
    sendToChannel() { throw new Error('not used'); }
  };
  class DetachedEntrySpot {}
  const fixture = await createEntryFixture(DetachedEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const outbound = new framework.DefaultZLinkSpotOutbound(
    serial,
    channelClient,
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    'test-mesh'
  );

  // Detached path: the continuation is registered outside the handler turn.
  const continuation = outbound.requestToChannel('api', 'ping').submit().then((reply) => {
    events.push(`continuation:${reply}:executing=${serial.isExecuting}`);
  });
  await delay(5);
  assert.deepEqual(events, ['request:submitted']);

  // While the reply is delivered, a competing entry callback is queued; the
  // continuation must enter the serial line, not run as a bare microtask.
  let releaseBusy;
  const busyGate = new Promise((resolve) => {
    releaseBusy = resolve;
  });
  const busy = serial.execute(async () => {
    events.push('busy:start');
    await busyGate;
    events.push('busy:end');
  });
  await delay(2);
  resolveReply('pong');
  await delay(5);
  // The busy turn holds the line, so the continuation has not run yet.
  assert.deepEqual(events, ['request:submitted', 'busy:start']);
  releaseBusy();
  await Promise.all([busy, continuation]);
  assert.deepEqual(events, [
    'request:submitted',
    'busy:start',
    'busy:end',
    'continuation:pong:executing=true'
  ]);
});

test('request submit keeps the entry turn until its reply completes', async () => {
  const events = [];
  const channelClient = {
    requestToChannel(_channelName, request) {
      return {
        packetName() { return this; },
        timeout() { return this; },
        async submit() {
          events.push(`request:${request}`);
          await delay(10);
          return `reply:${request}`;
        }
      };
    },
    sendToChannel() { throw new Error('not used'); }
  };
  class GatedEntrySpot {}
  const fixture = await createEntryFixture(GatedEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const outbound = new framework.DefaultZLinkSpotOutbound(
    serial, channelClient, undefined, undefined, undefined, undefined, undefined, 'test-mesh'
  );

  const gated = serial.execute(async () => {
    events.push('handler:start');
    const reply = await outbound.requestToChannel('api', 'ping').submit();
    events.push(`handler:${reply}`);
  });
  const next = serial.execute(() => {
    events.push('next');
  });
  await delay(2);
  assert.deepEqual(events, ['handler:start', 'request:ping']);
  await Promise.all([gated, next]);

  assert.deepEqual(events, ['handler:start', 'request:ping', 'handler:reply:ping', 'next']);
});

test('same-Spot awaited requests fail before transport while self-send remains FIFO-admitted', async () => {
  const submissions = [];
  const addressTransport = {
    async sendToSpotAddress(spotId, message) {
      submissions.push({ kind: 'send', spotId, message });
      return { status: 'submitted' };
    },
    async requestToSpotAddress(spotId, request) {
      submissions.push({ kind: 'request', spotId, request });
      return { marker: 'unexpected-reply' };
    }
  };
  const serial = new framework.ZLinkSpotSerialExecutor(true, 'same-spot');
  const outbound = new framework.DefaultZLinkSpotOutbound(
    serial,
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    'test-mesh',
    undefined,
    addressTransport
  );
  const isInvalidOperation = (error) => error instanceof framework.ZLinkFrameworkException
    && error.kind === framework.ZLinkFrameworkErrorKind.InvalidOperation;

  assert.throws(
    () => outbound.requestToSpot('same-spot', { requestId: 'async' }).submit(),
    isInvalidOperation
  );
  await serial.execute(async () => {
    assert.throws(
      () => outbound.requestToSpot('same-spot', { requestId: 'yield' }).yield(),
      isInvalidOperation
    );
  });

  await outbound.sendToSpot('same-spot', { requestId: 'send', marker: 'self-send' }).submit();
  assert.deepEqual(submissions.map(({ kind }) => kind), ['send']);
});

test('yield request from an entry turn is rejected because Entry forbids Yield', async () => {
  // 04-async-execution-policy.ko.md SS1.1: "PerActor와 Entry에서 Yield가
  // 금지되는 기존 규칙도 바뀌지 않는다." spot-entry-activation.ts
  // constructs its serial executor with yieldAllowed=false
  // (`new ZLinkSpotSerialExecutor(undefined, 'entry', false)`, 787d173c40)
  // to enforce exactly this, so `.yield()` from an Entry Spot turn must
  // reject synchronously and never reach the outbound submit.
  const events = [];
  const channelClient = {
    requestToChannel(_channelName, request) {
      return {
        packetName() { return this; },
        timeout() { return this; },
        submit() {
          events.push(`request:${request}`);
          return Promise.resolve('reply:ping');
        }
      };
    },
    sendToChannel() { throw new Error('not used'); }
  };
  class YieldEntrySpot {}
  const fixture = await createEntryFixture(YieldEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const outbound = new framework.DefaultZLinkSpotOutbound(
    serial, channelClient, undefined, undefined, undefined, undefined, undefined, 'test-mesh'
  );

  await assert.rejects(
    serial.execute(async () => {
      events.push('handler:start');
      await outbound.requestToChannel('api', 'ping').yield();
    }),
    (error) => error instanceof framework.ZLinkConfigurationException
      && /yield requires a SpotWide User Spot or Instance Spot application handler/.test(error.message)
  );
  assert.deepEqual(events, ['handler:start']);
});

test('yield from an injected outbound is still gated by the ambient entry turn', async () => {
  // The outbound's own bound serial executor is irrelevant here: yield()
  // captures the *ambient* turn via AsyncLocalStorage
  // (execution/index.ts captureZLinkSpotSerialTurn), so an outbound built
  // on a default (yieldAllowed=true) executor still rejects when invoked
  // from inside an Entry Spot's turn.
  const events = [];
  const channelClient = {
    requestToChannel(_channelName, request) {
      return {
        packetName() { return this; },
        timeout() { return this; },
        submit() {
          events.push(`request:${request}`);
          return Promise.resolve('reply:ping');
        }
      };
    },
    sendToChannel() { throw new Error('not used'); }
  };
  class YieldEntrySpot {}
  const fixture = await createEntryFixture(YieldEntrySpot);
  const ownerSerial = fixture.activation.serialExecutor;
  const injectedOutbound = new framework.DefaultZLinkSpotOutbound(
    new framework.ZLinkSpotSerialExecutor(),
    channelClient,
    undefined,
    undefined,
    undefined,
    undefined,
    undefined,
    'test-mesh'
  );

  await assert.rejects(
    ownerSerial.execute(async () => {
      events.push('handler:start');
      await injectedOutbound.requestToChannel('api', 'ping').yield();
    }),
    (error) => error instanceof framework.ZLinkConfigurationException
      && /yield requires a SpotWide User Spot or Instance Spot application handler/.test(error.message)
  );
  assert.deepEqual(events, ['handler:start']);
});

test('runIoWorker promise continuation re-enters the owning spot serial executor', async () => {
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const context = fixture.activation.context;
  const events = [];

  let releaseBusy;
  const busyGate = new Promise((resolve) => {
    releaseBusy = resolve;
  });
  let completed;
  const completion = new Promise((resolve) => {
    completed = resolve;
  });

  const busy = serial.execute(async () => {
    events.push('busy:start');
    void context.runIoWorker(async () => {
      events.push('work');
      return 21 * 2;
    }).submit().then((result) => {
      events.push(`completed:${result}:executing=${serial.isExecuting}`);
      completed();
    });
    await busyGate;
    events.push('busy:end');
  });
  // The promise continuation resumes through the captured Spot turn.
  await delay(15);
  assert.deepEqual(events, ['busy:start', 'work', 'completed:42:executing=true']);
  releaseBusy();
  await Promise.all([busy, completion]);
  assert.deepEqual(events, ['busy:start', 'work', 'completed:42:executing=true', 'busy:end']);
});

test('Spot context splits CPU and I/O worker operations by execution kind', async () => {
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const context = fixture.activation.context;

  assert.equal(typeof context.runCpuWorker, 'function');
  assert.equal(typeof context.runIoWorker, 'function');
  assert.equal(typeof context.runWorker, 'undefined');
});

test('runCpuWorker executes synchronous work on a worker thread', async () => {
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const mainThreadId = require('node:worker_threads').threadId;

  const workerThreadId = await fixture.activation.context
    .runCpuWorker(() => require('node:worker_threads').threadId)
    .submit();

  assert.notEqual(workerThreadId, mainThreadId);
});

test('runCpuWorker reuses an idle worker until the configured idle timeout', async () => {
  const worker = new framework.ZLinkWorkerRuntime({
    minThreads: 1,
    maxThreads: 1,
    idleTimeoutMs: 100,
    maxQueueLength: 4
  });
  const serial = new framework.ZLinkSpotSerialExecutor();
  const firstThreadId = await cpuWorkerCall(
    worker,
    serial,
    () => require('node:worker_threads').threadId
  ).submit();
  const secondThreadId = await cpuWorkerCall(
    worker,
    serial,
    () => require('node:worker_threads').threadId
  ).submit();

  assert.equal(secondThreadId, firstThreadId);
});

test('runCpuWorker rejects async work and enforces its timeout', async () => {
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const context = fixture.activation.context;

  await assert.rejects(
    () => context.runCpuWorker(async () => 'wrong-kind').submit(),
    /requires a synchronous function/
  );
  await assert.rejects(
    () => context.runCpuWorker(() => {
      const deadline = Date.now() + 50;
      while (Date.now() < deadline) { /* occupy the CPU worker */ }
      return 'late';
    }).timeoutMs(5).submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
});

test('runIoWorker async keeps the current Spot turn until worker completion', async () => {
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const context = fixture.activation.context;
  const events = [];

  const gated = serial.execute(async () => {
    events.push('handler:start');
    const result = await context.runIoWorker(async () => {
      await delay(5);
      return 'done';
    }).submit();
    events.push(`handler:${result}`);
  });
  const next = serial.execute(() => {
    events.push('next');
  });
  await Promise.all([gated, next]);

  assert.deepEqual(events, ['handler:start', 'handler:done', 'next']);
});

test('runIoWorker yield from an entry turn is rejected because Entry forbids Yield', async () => {
  // Same Entry-forbids-Yield rule as the outbound yield tests above; here it
  // is DefaultZLinkWorkerCall.yield() (workers/index.ts) that must reject.
  class WorkerEntrySpot {}
  const fixture = await createEntryFixture(WorkerEntrySpot);
  const serial = fixture.activation.serialExecutor;
  const context = fixture.activation.context;
  const events = [];

  await assert.rejects(
    serial.execute(async () => {
      events.push('handler:start');
      await context.runIoWorker(async () => 'done').yield();
    }),
    (error) => error instanceof framework.ZLinkConfigurationException
      && /yield requires a SpotWide User Spot or Instance Spot application handler/.test(error.message)
  );
  assert.deepEqual(events, ['handler:start']);
});

test('runIoWorker async also works when the call is created outside a Spot turn', async () => {
  const worker = new framework.ZLinkWorkerRuntime();
  const serial = new framework.ZLinkSpotSerialExecutor();
  const call = ioWorkerCall(worker, serial, async () => 'done');

  assert.equal(await call.submit(), 'done');
});

test('runCpuWorker queue full fails fast with WorkerQueueFull and does not block the dispatcher', async () => {
  const worker = new framework.ZLinkWorkerRuntime({ maxThreads: 1, maxQueueLength: 1 });
  const serial = new framework.ZLinkSpotSerialExecutor();
  const longCall = cpuWorkerCall(worker, serial, () => {
    const deadline = Date.now() + 100;
    while (Date.now() < deadline) { /* occupy the CPU worker */ }
    return 'long';
  });
  const longJob = longCall.submit();
  await waitFor(() => worker.inFlightCount === 1);
  const queuedCall = cpuWorkerCall(worker, serial, () => 'queued');
  const queuedJob = queuedCall.submit();

  const overflowCall = cpuWorkerCall(worker, serial, () => 'overflow');
  const startedAt = Date.now();
  await assert.rejects(
    () => overflowCall.submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.CapacityExceeded
  );
  assert.equal(Date.now() - startedAt < 100, true);

  // The owning dispatcher keeps processing while the queue is saturated.
  const dispatched = await serial.execute(() => 'dispatcher-alive');
  assert.equal(dispatched, 'dispatcher-alive');

  const errors = [];
  const overflowCallback = cpuWorkerCall(worker, serial, () => 'overflow');
  void overflowCallback.submit().then(
    () => errors.push('completed'),
    (error) => serial.execute(() => errors.push(`error:${error.kind}:executing=${serial.isExecuting}`))
  );
  await delay(10);
  assert.deepEqual(errors, [`error:${framework.ZLinkFrameworkErrorKind.CapacityExceeded}:executing=true`]);

  assert.equal(await longJob, 'long');
  assert.equal(await queuedJob, 'queued');
});

test('runCpuWorker cancellation removes a queued job before it consumes a worker slot', async () => {
  const worker = new framework.ZLinkWorkerRuntime({ maxThreads: 1, maxQueueLength: 2 });
  const serial = new framework.ZLinkSpotSerialExecutor();
  const longJob = cpuWorkerCall(worker, serial, () => {
    const deadline = Date.now() + 80;
    while (Date.now() < deadline) { /* occupy the CPU worker */ }
    return 'long';
  }).submit();
  await waitFor(() => worker.inFlightCount === 1);

  const controller = new AbortController();
  const cancelledJob = cpuWorkerCall(worker, serial, () => 'cancelled').submit(controller.signal);
  controller.abort();
  await assert.rejects(
    () => cancelledJob,
    (error) => error?.name === 'AbortError'
  );

  const nextJob = cpuWorkerCall(worker, serial, () => 'next').submit();
  assert.equal(await longJob, 'long');
  assert.equal(await nextJob, 'next');
});

test('runIoWorker timeout fails the caller and drops the late completion without user callbacks', async () => {
  const worker = new framework.ZLinkWorkerRuntime({ maxThreads: 2, maxQueueLength: 16 });
  const serial = new framework.ZLinkSpotSerialExecutor();
  const events = [];

  let observedSignal;
  const submitCall = ioWorkerCall(worker, serial, async (signal) => {
    observedSignal = signal;
    await delay(40);
    return 'late';
  }).timeoutMs(10);
  await assert.rejects(
    () => submitCall.submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.DeadlineExceeded
  );
  assert.equal(observedSignal.aborted, true);

  const callbackCall = ioWorkerCall(worker, serial, async () => {
    await delay(40);
    return 'late';
  }).timeoutMs(10);
  void callbackCall.submit().then(
    (result) => events.push(`completed:${result}`),
    (error) => events.push(`error:${error.kind}`)
  );
  await delay(80);
  // The timeout error reached onError; the late completion fired no callback.
  assert.deepEqual(events, [`error:${framework.ZLinkFrameworkErrorKind.DeadlineExceeded}`]);
});

test('runIoWorker work failure surfaces as WorkerFailed wrapping the cause', async () => {
  const worker = new framework.ZLinkWorkerRuntime();
  const serial = new framework.ZLinkSpotSerialExecutor();
  const cause = new Error('disk full');
  const call = ioWorkerCall(worker, serial, async () => {
    throw cause;
  });
  await assert.rejects(
    () => call.submit(),
    (error) =>
      error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.InternalFailure
      && error.cause === cause
  );
});

test('runIoWorker call accepts only one terminator', async () => {
  const worker = new framework.ZLinkWorkerRuntime({ maxThreads: 1, maxQueueLength: 16 });
  const serial = new framework.ZLinkSpotSerialExecutor();

  const asyncFirst = ioWorkerCall(worker, serial, async () => 'done');
  const pending = asyncFirst.submit();
  assert.throws(
    () => asyncFirst.submit(),
    (error) =>
      error instanceof framework.ZLinkConfigurationException
      && /only one terminator/.test(error.message)
  );
  assert.equal(await pending, 'done');

  let submitted = false;
  const submitFirst = ioWorkerCall(worker, serial, async () => {
    submitted = true;
    return 'done';
  });
  // submit() always returns Promise<T> (ZLinkWorkerCall<T> per
  // contracts/Spots/Contracts.ts) -- in practice a thenable
  // ZLinkSerialDeliveredPromise, not a native Promise instance. The second
  // call is the "only one terminator" case under test, not the first.
  const submitFirstPending = submitFirst.submit();
  assert.equal(typeof submitFirstPending.then, 'function');
  assert.throws(
    () => submitFirst.submit(),
    (error) =>
      error instanceof framework.ZLinkConfigurationException
      && /only one terminator/.test(error.message)
  );
  await waitFor(() => submitted);
  assert.equal(await submitFirstPending, 'done');

  // yield() requires an active SpotWide turn (requireZLinkYieldTurn,
  // execution/index.ts), unlike submit(), so this leg has to run inside
  // serial.execute(...) instead of calling yield() at module scope.
  const yieldFirst = ioWorkerCall(worker, serial, async () => 'done');
  const yieldResult = await serial.execute(async () => {
    const yielded = yieldFirst.yield();
    assert.throws(
      () => yieldFirst.submit(),
      (error) =>
        error instanceof framework.ZLinkConfigurationException
        && /only one terminator/.test(error.message)
    );
    return await yielded;
  });
  assert.equal(yieldResult, 'done');
});

test('worker options are accepted on the framework options builder and validated', () => {
  const options = framework.createFrameworkOptions((builder) => {
    builder.configureWorker({
      minThreads: 0,
      maxThreads: 8,
      idleTimeoutMs: 30_000,
      maxQueueLength: 1024
    });
  });
  assert.deepEqual(options.worker, {
    minThreads: 0,
    maxThreads: 8,
    idleTimeoutMs: 30_000,
    maxQueueLength: 1024
  });
  const registration = framework.createFrameworkRegistration(options);
  assert.deepEqual(registration.worker, options.worker);

  for (const invalid of [
    { minThreads: -1, maxThreads: 1, idleTimeoutMs: 0, maxQueueLength: 1 },
    { maxThreads: 0 },
    { minThreads: 2, maxThreads: 1, idleTimeoutMs: 0, maxQueueLength: 1 },
    { idleTimeoutMs: -1 },
    { maxQueueLength: 0 }
  ]) {
    assert.throws(
      () => framework.createFrameworkRegistration({ worker: invalid }),
      framework.ZLinkConfigurationException
    );
  }
});

test('worker declarations expose split CPU and I/O execution contracts', () => {
  const declarationsRoot = path.resolve(__dirname, '../../packages/framework/dist/contracts');
  const declarations = readTree(declarationsRoot);
  assert.match(declarations, /runCpuWorker<T>\(work: \(signal: AbortSignal\) => T\): ZLinkWorkerCall<T>/);
  assert.match(declarations, /runIoWorker<T>\(work: \(signal: AbortSignal\) => Promise<T>\): ZLinkWorkerCall<T>/);
  assert.doesNotMatch(declarations, /\brunWorker<T>/);
});

function ioWorkerCall(worker, serial, work) {
  return new framework.DefaultZLinkWorkerCall(
    serial,
    (timeoutMs, signal) => worker.scheduleIo(work, timeoutMs, signal)
  );
}

function cpuWorkerCall(worker, serial, work) {
  return new framework.DefaultZLinkWorkerCall(
    serial,
    (timeoutMs, signal) => worker.scheduleCpu(work, timeoutMs, signal)
  );
}

async function waitFor(predicate) {
  const deadline = Date.now() + 1000;
  while (!predicate()) {
    if (Date.now() >= deadline) throw new Error('condition timed out');
    await delay(1);
  }
}

function readTree(root) {
  const parts = [];
  visit(root);
  return parts.join('\n');

  function visit(current) {
    for (const entry of fs.readdirSync(current, { withFileTypes: true })) {
      const fullPath = path.join(current, entry.name);
      if (entry.isDirectory()) {
        visit(fullPath);
      } else if (entry.isFile() && fullPath.endsWith('.d.ts')) {
        parts.push(fs.readFileSync(fullPath, 'utf8'));
      }
    }
  }
}
