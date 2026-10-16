const assert = require('node:assert/strict');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');

const ANY_LOOPBACK_PORT = 'tcp://127.0.0.1:*';
const RECEIVE_ROTATION_FRAMES = /\b(pumpBatch|receiveOne|processReceived|tickLiveness|onReadable)\b/;

test.afterEach(async () => {
  await new Promise((resolve) => setTimeout(resolve, 0));
});

// Spec 08 §4.3 W1–W3 and Submit and completion §11: the handler starts in a
// new execution turn of the application worker. The receive rotation and its
// management work have returned before any handler instruction runs.
test('node-direct handlers run after the receive rotation turn, never on its stack', async () => {
  const observations = [];
  let receiveTurn = false;
  let managementTurn = false;
  const originalPumpBatch = RawServiceMeshRuntime.prototype.pumpBatch;
  const originalTickLiveness = RawServiceMeshRuntime.prototype.tickLiveness;
  RawServiceMeshRuntime.prototype.pumpBatch = function (...args) {
    receiveTurn = true;
    try {
      return originalPumpBatch.apply(this, args);
    } finally {
      receiveTurn = false;
    }
  };
  RawServiceMeshRuntime.prototype.tickLiveness = function (...args) {
    managementTurn = true;
    try {
      return originalTickLiveness.apply(this, args);
    } finally {
      managementTurn = false;
    }
  };
  const observe = () => {
    const limit = Error.stackTraceLimit;
    Error.stackTraceLimit = 200;
    const stack = new Error().stack;
    Error.stackTraceLimit = limit;
    observations.push({ receiveTurn, managementTurn, stack });
  };
  const pair = await startPair({
    request: (value) => {
      observe();
      return { value: value.value };
    },
    send: () => observe()
  });
  try {
    for (let index = 0; index < 8; index++) {
      assert.deepEqual(await pair.request({ value: `sync-${index}` }), { value: `sync-${index}` });
      await pair.send({ value: 'one-way' });
    }
    await waitUntil(() => observations.length >= 16);
    for (const observation of observations) {
      assert.equal(observation.receiveTurn, false, 'handler ran inside the receive rotation');
      assert.equal(observation.managementTurn, false, 'handler ran inside management work');
      assert.doesNotMatch(observation.stack, RECEIVE_ROTATION_FRAMES);
    }
  } finally {
    RawServiceMeshRuntime.prototype.pumpBatch = originalPumpBatch;
    RawServiceMeshRuntime.prototype.tickLiveness = originalTickLiveness;
    await pair.stop();
  }
});

test('a handler that throws, rejects, or awaits does not stall the dispatch pump', async () => {
  let sends = 0;
  const pair = await startPair({
    request: async (value) => {
      if (value.value === 'throw') throw new Error('handler threw');
      if (value.value === 'await') await new Promise((resolve) => setTimeout(resolve, 20));
      if (value.value === 'reject') return Promise.reject(new Error('handler rejected'));
      return { value: value.value };
    },
    send: (value) => {
      sends++;
      if (value.value === 'throw') throw new Error('send handler threw');
    }
  });
  try {
    await assert.rejects(pair.request({ value: 'throw-sync' }), /synchronous handler throw/);
    await assert.rejects(pair.request({ value: 'throw' }), /handler threw/);
    await assert.rejects(pair.request({ value: 'reject' }), /handler rejected/);
    await pair.send({ value: 'throw' });
    const results = await Promise.all([
      pair.request({ value: 'await' }),
      pair.request({ value: 'ok-1' }),
      pair.request({ value: 'ok-2' })
    ]);
    assert.deepEqual(results, [{ value: 'await' }, { value: 'ok-1' }, { value: 'ok-2' }]);
    await pair.send({ value: 'after' });
    await waitUntil(() => sends >= 2);
    assert.deepEqual(await pair.request({ value: 'last' }), { value: 'last' });
  } finally {
    await pair.stop();
  }
});

async function startPair(behavior, onWaitTurn) {
  class TurnPing {}
  class TurnRequestHandler {
    handle(value) {
      if (value.value === 'throw-sync') throw new Error('synchronous handler throw');
      return behavior.request(value);
    }
  }
  class TurnSendHandler {
    handle(value) {
      return behavior.send(value);
    }
  }
  const targetRegistration = framework.createFrameworkRegistration({
    spotNodes: {
      mesh: {
        router: { routingId: 'turn-target', bind: ANY_LOOPBACK_PORT },
        routeRequestHandlers: [{ packetName: 'TurnPing', handlerType: TurnRequestHandler }],
        routeSendHandlers: [{ packetName: 'TurnPing', handlerType: TurnSendHandler }]
      }
    }
  });
  // Without a provider resolver the handler instance is prepared once and
  // activated without a per-message scope, which is the dispatch path measured here.
  const target = new framework.ZLinkFrameworkRuntimeHost({ registration: targetRegistration });
  await target.start();
  const endpoint = target.requirePrimaryMeshNode().status().localEndpoint;
  const options = framework.createFrameworkOptions((builder) => {
    builder
      .addRouteMesh('mesh')
      .listen(ANY_LOOPBACK_PORT)
      .routingId('turn-caller')
      .peerConnections()
      .connect('turn-target', endpoint);
  });
  const registration = framework.createFrameworkRegistration(options);
  const caller = new framework.ZLinkFrameworkRuntimeHost({ registration });
  const client = new framework.DefaultZLinkRouteClient(
    registration,
    caller.routeTransport,
    caller.spotRouterChannelIdForMesh
  );
  await caller.start();
  await waitUntil(() =>
    caller.routeMeshRuntime
      .snapshot('mesh')
      .peers.some(
        (peer) =>
          String(peer.nodeRid) === 'turn-target' && peer.state === framework.ZLinkPeerState.Ready
      ),
    onWaitTurn
  );
  const packet = (value) => Object.assign(new TurnPing(), value);
  return {
    request: (value) =>
      client.requestToNode('mesh', 'turn-target', packet(value)).timeout(5000).submit(),
    send: (value) => client.sendToNode('mesh', 'turn-target', packet(value)).submit(),
    stop: async () => {
      await caller.stop();
      await target.stop();
    }
  };
}

async function waitUntil(predicate, onTurn) {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if (predicate()) return;
    onTurn?.();
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail('Condition did not become true before the deadline.');
}

// Spec 08 §4.3: no per-record dispatch task or wrapper. A one-way record with
// a synchronous handler adds no Framework Promise between the pump and the
// handler; what remains is the worker's claim drain and the source submit.
test('one-way node-direct dispatch adds no per-record Promise wrappers', async (t) => {
  const { createHook } = require('node:async_hooks');
  let handled = 0;
  const pair = await startPair({ request: (value) => value, send: () => void handled++ });
  let counting = false;
  let promises = 0;
  const hook = createHook({
    init(_id, type) {
      if (!counting || type !== 'PROMISE') return;
      // Only Promises the Framework runtime creates; the test's own waits
      // and the application handler are not part of the dispatch path.
      const frames = new Error().stack.split('\n').slice(2);
      const creator = frames.find((frame) => !frame.includes('node:internal'));
      if (creator !== undefined && creator.includes('/packages/framework/dist/runtime/')) promises++;
    }
  }).enable();
  try {
    for (let index = 0; index < 8; index++) await pair.send({ value: 'warm' });
    await waitUntil(() => handled >= 8);
    const messages = 64;
    counting = true;
    for (let index = 0; index < messages; index++) await pair.send({ value: 'count' });
    await waitUntilImmediate(() => handled >= 8 + messages);
    counting = false;
    const perMessage = promises / messages;
    t.diagnostic(`PROMISE per one-way message: ${perMessage}`);
    assert.ok(perMessage <= 4, `PROMISE per one-way message: ${perMessage} > 4`);
  } finally {
    hook.disable();
    await pair.stop();
  }
});

async function waitUntilImmediate(predicate) {
  for (let attempt = 0; attempt < 100_000; attempt++) {
    if (predicate()) return;
    await new Promise((resolve) => setImmediate(resolve));
  }
  assert.fail('Condition did not become true.');
}

// Spec 08 §4.2: management work runs at its deadline, at most once per
// receive rotation, and the idle wait lasts until the nearest deadline. With
// timers mocked, an idle connected pair passes 200 ms of virtual time without
// a fixed-interval maintenance timer firing; the armed timer waits for the
// next liveness deadline.
test('an idle connected node waits for its next management deadline', async (t) => {
  t.mock.timers.enable({ apis: ['setTimeout'] });
  // Requests and teardown may wait for the pump's timer yield. Advance the
  // same virtual clock while those operations join; the idle observation below
  // advances exactly its own 200 ticks.
  const withClock = async (operation) => {
    let settled = false;
    const completion = operation.finally(() => { settled = true; });
    void completion.catch(() => undefined);
    await waitUntilImmediate(() => {
      if (!settled) t.mock.timers.tick(1);
      return settled;
    });
    return completion;
  };
  const armed = [];
  const { setTimeout: mockedSetTimeout } = globalThis;
  t.mock.method(globalThis, 'setTimeout', function (callback, delay, ...args) {
    if (typeof callback === 'function' && callback.name === 'onMaintenance') armed.push(delay);
    return mockedSetTimeout.call(this, callback, delay, ...args);
  });
  // Virtual time moves one millisecond per event-loop turn while the pair
  // connects, which keeps the handshake cadence of a pending peer running.
  const pair = await startPair({ request: (value) => value, send: () => undefined }, () =>
    t.mock.timers.tick(1)
  );
  try {
    assert.deepEqual(await withClock(pair.request({ value: 'ready' })), { value: 'ready' });
    for (let turn = 0; turn < 10; turn++) await new Promise((resolve) => setImmediate(resolve));
    armed.length = 0;
    for (let elapsed = 0; elapsed < 200; elapsed++) {
      t.mock.timers.tick(1);
      await new Promise((resolve) => setImmediate(resolve));
    }
    t.diagnostic(`maintenance timer delays armed in 200 ms of idle virtual time: ${armed}`);
    // No fixed-interval timer: anything armed waits for a liveness or request
    // deadline seconds away, not the 1 ms maintenance cadence.
    assert.ok(armed.length <= 2, `maintenance timers armed: ${armed.length}`);
    for (const delay of armed) assert.ok(delay >= 1_000, `maintenance timer delay ${delay} ms`);
    assert.deepEqual(await withClock(pair.request({ value: 'after-idle' })), { value: 'after-idle' });
  } finally {
    await withClock(pair.stop());
    t.mock.timers.reset();
  }
});
