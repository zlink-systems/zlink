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
    const pending = runtime.requestToNode('not-admitted', payload(), 1000, undefined, observer, 7);
    returnedBeforeTerminal = observed.length === 0;
    assert.equal(pending.promise, undefined);
    return { high: 1n, low: pending.id };
  });
  const terminal = await completion;
  assert.equal(returnedBeforeTerminal, true);
  assert.equal(observed.length, 1);
  assert.equal(observed[0].operationKind, 7);
  assert.notEqual(terminal.terminalResult, 0);
  assert.equal(table.pendingCount, 0);
  runtime.mailbox.close();
});

test('an observed remote request registers once without a registry Promise', async () => {
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
      return new Promise(() => {});
    }
  };
  runtime.topology.admittedPeer = () => ({ descriptor: { nodeRoutingId: 'target' } });
  runtime.isPeerRouteReady = () => true;
  try {
    const observer = { operationCompleted() {}, operationFailed() {} };
    const pending = runtime.requestToNode('target', payload(), 1000, undefined, observer, 1);
    assert.equal(pending.promise, undefined);
    assert.equal(promiseRegistrations, 0);
    assert.equal(observedRegistrations, 1);
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
