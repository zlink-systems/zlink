const { ZLinkRequestResult } = require('../../packages/framework/dist/runtime/messaging/request-result');
const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ApplicationJobQueue,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/runtime/host/application-job-queue');
const {
  RawServiceMeshRuntime
} = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const {
  OperationRegistry
} = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const {
  ZLinkMeshCompletionTable
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  SERVICE_WIRE_REQUIRED_CAPABILITY
} = require('../../packages/framework/dist/runtime/foundation/service-wire-constants.generated');
const {
  encodeApplicationPayload
} = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function createRuntime() {
  return new RawServiceMeshRuntime({
    applicationJobQueue: new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 16n }, () => 1n)
    ),
    descriptor: {
      meshName: 'mesh',
      nodeRoutingId: 'source',
      advertisedEndpoint: 'tcp://127.0.0.1:1',
      channels: [],
      state: 'serving',
      securityIdentity: 'test',
      applicationVersion: 0n,
      protocolCapabilities: [SERVICE_WIRE_REQUIRED_CAPABILITY],
      objectRole: 'server',
      placementWeight: 1,
      lifecycleGeneration: 1n,
      descriptorRevision: 1n,
      activeCapacityLimit: 10,
      pendingCapacityLimit: 10,
      activeCapacityUsed: 0,
      pendingCapacityUsed: 0
    }
  });
}

function payload() {
  return encodeApplicationPayload({
    packetName: 'Ping',
    contentType: 'application/octet-stream',
    payload: Buffer.from('ping')
  });
}

// Spec 08 E3–E5 and Submit and completion §10: the Framework completion slot
// is registered before submit, and the operation owner's atomic take hands
// the terminal straight to it. A terminal the request call decides itself
// reaches the observer only after the call returned the operation id, so the
// completion slot is already indexed under that id.
test('an observed request decided inside the call reaches the completion slot after registration', async () => {
  const runtime = createRuntime();
  const table = new ZLinkMeshCompletionTable();
  const observed = [];
  const observer = {
    operationCompleted(id, result, operationKind) {
      observed.push({ id, operationKind });
      table.complete({
        operationId: { high: 1n, low: id },
        terminalResult: result.terminalResult,
        failureErrno: result.failureCode,
        operationKind,
        kindData: null,
        parts: []
      });
    },
    operationFailed(id) {
      assert.fail(`operation ${id} failed`);
    }
  };
  let returnedBeforeTerminal = false;
  const completion = table.submit(() => {
    const pending = runtime.requestToNode('not-admitted', payload(), 1000, undefined, observer, 7, new ZLinkRequestResult());
    returnedBeforeTerminal = observed.length === 0;
    assert.equal(pending.promise, undefined);
    return { high: 1n, low: pending.id };
  }).pending;
  const terminal = await completion;
  assert.equal(returnedBeforeTerminal, true);
  assert.equal(observed.length, 1);
  assert.equal(observed[0].operationKind, 7);
  assert.notEqual(terminal.terminalResult, 0);
  assert.equal(table.pendingCount, 0);
  runtime.mailbox.close();
});

test('an observed remote request leaves terminal ownership in the completion entry', async () => {
  const runtime = createRuntime();
  let promiseRegistrations = 0;
  let observedRegistrations = 0;
  const register = OperationRegistry.prototype.register;
  const registerObserved = OperationRegistry.prototype.registerObserved;
  OperationRegistry.prototype.register = function (...args) {
    promiseRegistrations++;
    return register.apply(this, args);
  };
  OperationRegistry.prototype.registerObserved = function (...args) {
    observedRegistrations++;
    return registerObserved.apply(this, args);
  };
  let replyParts;
  runtime.router = {
    request(_target, parts) {
      replyParts = parts;
    }
  };
  runtime.topology.admittedPeer = () => ({ descriptor: { nodeRoutingId: 'target' } });
  runtime.isPeerRouteReady = () => true;
  try {
    const observer = { operationCompleted() {}, operationFailed() {} };
    const pending = runtime.requestToNode('target', payload(), 1000, undefined, observer, 1, new ZLinkRequestResult());
    assert.equal(pending.promise, undefined);
    assert.equal(promiseRegistrations, 0);
    assert.equal(observedRegistrations, 0);
    assert.equal(runtime.pendingOperationCount, 0);
    assert.equal(replyParts.length, 2);
    // The Promise contract stays for callers that do not observe.
    const plain = runtime.requestToNode('target', payload(), 1000);
    assert.ok(plain.promise instanceof Promise);
    assert.equal(promiseRegistrations, 1);
  } finally {
    OperationRegistry.prototype.register = register;
    OperationRegistry.prototype.registerObserved = registerObserved;
    runtime.mailbox.close();
  }
});

test('observed registry terminals reach the observer exactly once on reply, timeout and close', () => {
  const now = { value: 0 };
  const registry = new OperationRegistry({ now: () => now.value });
  const events = [];
  const observer = {
    operationCompleted: (id, value, context) => events.push(['completed', id, value, context]),
    operationFailed: (id, reason, context) => events.push(['failed', id, reason.name, context])
  };
  const replied = registry.registerObserved(100, observer, 'reply');
  const expired = registry.registerObserved(10, observer, 'timeout');
  const closed = registry.registerObserved(100, observer, 'close');
  assert.equal(registry.complete(replied, 'value'), true);
  assert.equal(registry.complete(replied, 'again'), false);
  now.value = 50;
  assert.equal(registry.expire(50, Number.POSITIVE_INFINITY), 1);
  registry.close();
  assert.deepEqual(events, [
    ['completed', replied, 'value', 'reply'],
    ['failed', expired, 'OperationTimeoutError', 'timeout'],
    ['failed', closed, 'OperationCancelledError', 'close']
  ]);
});

// Spec 08 §4.2: the I0 wait bound is the nearest management deadline.
test('registry and liveness report their next management deadline', () => {
  const {
    ServiceLivenessRegistry
  } = require('../../packages/framework/dist/runtime/foundation/service-liveness-registry');
  const now = { value: 0 };
  const registry = new OperationRegistry({ now: () => now.value });
  assert.equal(registry.nextDeadlineMs(), Number.POSITIVE_INFINITY);
  const late = registry.register(500);
  const early = registry.register(100);
  void late.promise.catch(() => undefined);
  void early.promise.catch(() => undefined);
  assert.equal(registry.nextDeadlineMs(), 100);
  registry.complete(early.id, 'done');
  // A completed entry leaves a lower bound until the next full expiry pass.
  assert.equal(registry.nextDeadlineMs(), 100);
  now.value = 100;
  assert.equal(registry.expire(100, Number.POSITIVE_INFINITY), 0);
  assert.equal(registry.nextDeadlineMs(), 500);
  registry.close();

  const liveness = new ServiceLivenessRegistry(5_000, 15_000);
  liveness.admit('peer', 'connection', 1_000);
  assert.equal(liveness.nextDeadlineMs(), 1_000);
  assert.equal(liveness.tick(1_000).probes.length, 1);
  assert.equal(liveness.nextDeadlineMs(), 6_000);
  const idle = liveness.tick(3_000);
  assert.equal(idle.probes.length + idle.timedOutNodes.length, 0);
  assert.equal(liveness.tick(6_000).probes.length, 1);
  assert.deepEqual(liveness.tick(16_000).timedOutNodes, ['peer']);
  assert.equal(liveness.nextDeadlineMs(), Number.POSITIVE_INFINITY);
});

function runtimeWithBinding(t, submit) {
  const zlink = require('@zlink-systems/zlink');
  const {
    ZLinkNodeRawBindingPort
  } = require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
  const context = zlink.createContext();
  const createRouter = zlink.createRouterSocket;
  t.mock.method(zlink, 'createRouterSocket', (ctx) => {
    const socket = createRouter(ctx);
    t.mock.method(socket, 'request', () => ({
      message() { return this; },
      timeout() { return this; },
      submit
    }));
    return socket;
  });
  const runtime = createRuntime();
  const host = new ZLinkNodeRawBindingPort(context).createHost();
  runtime.router = host.createRouter();
  runtime.topology.admittedPeer = () => ({ descriptor: { nodeRoutingId: 'target' } });
  t.after(() => {
    runtime.mailbox.close();
    host.close();
    context.close();
  });
  return runtime;
}

// Hot path §4.3 and Submit and completion §11 require the caller's new turn,
// but do not require a second Promise reaction between binding ownership and
// the operation owner's wire terminal.
test('binding reply ownership and the remote terminal share one Promise continuation', async (t) => {
  const { createHook } = require('node:async_hooks');
  const zlink = require('@zlink-systems/zlink');
  const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
  let releaseReply;
  let replyId;
  let callbacks = 0;
  const descendants = new Set();
  const hook = createHook({
    init(id, type, trigger) {
      if (type !== 'PROMISE') return;
      if (replyId === undefined) replyId = id;
      else if (trigger === replyId || descendants.has(trigger)) descendants.add(id);
    },
    before(id) {
      if (descendants.has(id)) callbacks++;
    }
  });
  hook.enable();
  const reply = new Promise((resolve) => { releaseReply = resolve; });
  hook.disable();
  const runtime = runtimeWithBinding(t, () => ({
    result: zlink.SubmitResult.Ok,
    admitted: Promise.resolve(),
    reply
  }));
  let complete;
  const done = new Promise((resolve) => { complete = resolve; });
  let terminal;
  try {
    hook.enable();
    runtime.requestToNode('target', payload(), 1000, undefined, {
      operationCompleted(_id, value) {
        terminal = value;
        hook.disable();
        complete();
      },
      operationFailed(_id, error) {
        hook.disable();
        complete();
        assert.fail(error);
      }
    }, 1, new ZLinkRequestResult());
    releaseReply([
      zlink.Message.from(wire.encodeReplyHeader(1n)),
      zlink.Message.from(payload())
    ]);
    await done;
    assert.equal(terminal.terminalResult, 0);
    assert.equal(callbacks, 1, 'reply ownership and terminal must not repeat the same result');
  } finally {
    hook.disable();
  }
});


for (const failureAt of ['binding', 'protocol', 'submit', 'admission', 'receive', 'retain', 'terminal']) {
  test(`remote terminal observes ${failureAt} failure without a rejected internal continuation`, async (t) => {
    const zlink = require('@zlink-systems/zlink');
    const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');
    const failure = new Error(`${failureAt} failed`);
    let releaseReply;
    let rejectReply;
    const reply = new Promise((resolve, reject) => { releaseReply = resolve; rejectReply = reject; });
    const runtime = runtimeWithBinding(t, () => {
      if (failureAt === 'submit') throw failure;
      return { result: zlink.SubmitResult.Ok, admitted: Promise.resolve(), reply };
    });
    runtime.topology.admittedPeer = () => ({
      descriptor: { nodeRoutingId: 'target' },
      liveness: { recordReceived() { if (failureAt === 'receive') throw failure; } }
    });
    let notify;
    const done = new Promise((resolve) => { notify = resolve; });
    const events = [];
    let returned = false;
    const table = new ZLinkMeshCompletionTable();
    const completion = table.submit(entry => {
    const operation = runtime.requestToNode('target', payload(), 1000,
      () => { if (failureAt === 'admission') throw failure; }, {
        operationCompleted(id, value) {
          table.complete({ operationId: { high: 1n, low: id }, terminalResult: value.terminalResult }, () => {
            events.push({ id, value, returned });
            notify();
            if (failureAt === 'terminal') throw failure;
            return { operationId: { high: 1n, low: id }, terminalResult: value.terminalResult,
              failureErrno: value.failureCode, operationKind: 1, kindData: null, parts: [] };
          });
        },
        operationFailed(id, error) {
          table.complete({ operationId: { high: 1n, low: id }, terminalResult: 1 }, () => {
            events.push({ id, error, returned }); notify();
            return { operationId: { high: 1n, low: id }, terminalResult: 1,
              failureErrno: 0, operationKind: 1, kindData: null, parts: [] };
          });
        }
      }, 1, entry);
      return { high: 1n, low: operation.id };
    });
    const delivered = completion.pending.catch(error => { assert.equal(error, failure); });
    returned = true;
    let closes = 0;
    if (failureAt === 'binding') rejectReply(failure);
    else if (!['submit', 'admission'].includes(failureAt)) {
      const replies = failureAt === 'protocol' ? [zlink.Message.from(Buffer.from('invalid'))] : [
        zlink.Message.from(wire.encodeReplyHeader(1n)), zlink.Message.from(payload())
      ];
      for (const message of replies) {
        const close = message.close.bind(message);
        t.mock.method(message, 'close', () => { closes++; close(); });
      }
      if (failureAt === 'retain') t.mock.method(replies[0], 'data', () => { throw failure; });
      releaseReply(replies);
    }
    await done;
    // Flush error observation; node:test rejects any unhandled rejection.
    await new Promise((resolve) => setImmediate(resolve));
    await delivered;
    table.dispose();
    assert.equal(events.length, 1);
    assert.equal(events[0].returned, true, 'operation id is returned before its terminal');
    if (failureAt === 'terminal') assert.equal(events[0].value.terminalResult, 0);
    else if (failureAt === 'protocol') assert.equal(events[0].error.name, 'ServiceWireProtocolError');
    else assert.equal(events[0].error, failure);
    assert.equal(runtime.operations.size, 0);
    assert.equal(closes, ['protocol'].includes(failureAt) ? 1 : ['receive', 'retain', 'terminal'].includes(failureAt) ? 2 : 0);
  });
}


for (const terminal of ['deadline', 'cancel', 'shutdown']) {
  test(`generic local request releases its capacity wait on ${terminal} through the shared entry`, async () => {
    const runtime = createRuntime();
    const table = new ZLinkMeshCompletionTable();
    const occupied = [];
    for (let i = 0; i < 16; i++) occupied.push(await runtime.applicationJobQueue.acquire(undefined, 'remote'));
    const controller = new AbortController();
    const observer = {
      operationCompleted(id, value, operationKind) {
        table.complete({ operationId: { high: 1n, low: id }, terminalResult: value.terminalResult,
          failureErrno: value.failureCode, operationKind, kindData: null, parts: [] });
      },
      operationFailed(id, error) {
        // A cancelled capacity acquisition is already terminal in this owner.
        table.complete({ operationId: { high: 1n, low: id }, terminalResult: 1,
          failureErrno: 0, operationKind: 1, kindData: null, parts: [] });
      }
    };
    try {
      const framework = require('../../packages/framework/dist/internal');
      const { ZLinkBackendResultError, RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
      let entry;
      const node = { requestToNode(_target, _parts, options) {
        entry = options.requestEntry;
        const operation = runtime.requestToNode('source', payload(), 1000, options.onAdmitted, observer, 1, entry);
        return { high: 1n, low: operation.id };
      } };
      const transport = new framework.ZLinkRuntimeRouteTransport(
        () => undefined, undefined,
        () => ({ meshNode: () => node, meshCompletionTable: () => table }),
        undefined, undefined, undefined, undefined, () => false
      );
      const registration = framework.createFrameworkRegistration({ spotNodes: { mesh: { router: { bind: 'inproc://local-entry-test' } } } });
      const client = new framework.DefaultZLinkRouteClient(registration, transport);
      class LocalQuery {}
      const reply = client.requestToNode('mesh', 'source', new LocalQuery()).submit(controller.signal);
      const failure = Promise.allSettled([reply, reply.admitted]);
      assert.equal(runtime.pendingOperationCount, 0);
      assert.equal(table.pendingCount, 1, table.pendingCount === 0 ? String((await failure)[0].reason) : 'pending terminal');
      assert.equal(runtime.applicationJobQueue.snapshot().capacityWaiters, 1n);
      if (terminal === 'deadline') assert.equal(table.expire(entry.deadlineMs), 1);
      else if (terminal === 'cancel') controller.abort(new DOMException('cancel local capacity wait', 'AbortError'));
      else table.dispose(new ZLinkBackendResultError('request', RequestResult.Terminated));
      const stages = await failure;
      assert.equal(stages[0].status, 'rejected');
      assert.equal(stages[1].status, 'rejected');
      assert.equal(stages[0].reason, stages[1].reason);
      if (terminal === 'deadline') assert.equal(stages[0].reason.kind, framework.ZLinkFrameworkErrorKind.DeadlineExceeded);
      else if (terminal === 'shutdown') assert.equal(stages[0].reason.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
      else assert.equal(stages[0].reason.name, 'AbortError');
      await new Promise(resolve => setImmediate(resolve));
      assert.equal(table.pendingCount, 0);
      assert.equal(runtime.applicationJobQueue.snapshot().capacityWaiters, 0n);
      assert.equal(runtime.mailbox.pendingMessages('application'), 0);
    } finally {
      table.dispose(); runtime.close();
      for (const permit of occupied) permit.releaseAfterInternalProcessing();
    }
  });
}
