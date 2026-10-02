'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const { execFile } = require('node:child_process');
const { promisify } = require('node:util');
const { OperationRegistry, OperationCancelledError, OperationTimeoutError } = require('../../packages/framework/dist/runtime/foundation/operation-registry');

for (const pending of [false, true, 'native']) {
  test(`maintenance process exits after ${pending ? `waiting for a delayed request reply (${pending === 'native' ? 'native' : 'handle-free adapter'})` : 'idle startup'}`, async () => {
    const child = `
      const backend = require('./packages/framework/dist/runtime/backend');
      const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('./packages/framework/dist/runtime/host/application-job-queue');
      const factory = new backend.ZLinkNodeBackendAdapterFactory();
      const context = factory.createChannelAdapter().createContext();
      const suffix = 'node-dispatch-r3-child-' + process.pid;
      const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 2n));
      const node = ${pending === 'native' ? `factory.createMeshAdapter().createMeshNode(context, { meshName: suffix, routingId: suffix, applicationJobQueue: queue })` : `
        new (require('./packages/framework/dist/runtime/backend/node/node-raw-mesh-backend').ZLinkNodeRawMeshBackend)(suffix, suffix, {
          createHost: () => ({
            createRouter: () => ({
              setRoutingId() {}, setReceiveFlowState() {}, bind() {},
              localEndpoint: () => 'tcp://127.0.0.1:54321',
              setReadableHandler() {}, routesSnapshot: () => [], receive: () => undefined,
              close() {}
            }), close() {}
          })
        }, queue)`};
      node.setBind('inproc://' + suffix);
      node.start();
      console.log('started');
      ${pending ? `
      const table = new backend.ZLinkMeshCompletionTable();
      const { ReceiveKind } = require('./packages/framework/dist/runtime/foundation/service-runtime-contracts');
      const { RequestResult } = require('./packages/framework/dist/runtime/backend/runtime-values');
      const pump = new backend.ZLinkMeshDispatchPump(node, {
        applicationJobQueue: queue,
        async dispatch(_owner, record) {
          if (record.kind === ReceiveKind.Completion) { table.complete(record); return; }
          await new Promise(resolve => setTimeout(resolve, 30).unref());
          await record.reply(Buffer.from('reply'));
        }
      });
      pump.start();
      table.submit(() => node.requestToNode(node.status().routingId, Buffer.from('request'), { timeoutMs: 1000 }))
        .then(completion => {
          if (completion.terminalResult !== RequestResult.Ok) throw new Error('reply failed');
          backend.closeMeshCompletion(completion);
          console.log('replied');
          table.dispose();
          return pump.dispose();
        }).then(() => { node.close(); return context.dispose(); });
      ` : ''}
    `;
    const result = await promisify(execFile)(process.execPath, ['-e', child], {
      cwd: require('node:path').resolve(__dirname, '../..'), timeout: 5000
    });
    assert.match(result.stdout, /started/);
    if (pending) assert.match(result.stdout, /replied/);
  });
}

test('pending operation registration allocates no native deadline timer', async () => {
  const transitions = [];
  const registry = new OperationRegistry(undefined, () => transitions.push(registry.size));
  let timers = 0;
  const hook = createHook({ init(_id, type) { if (type === 'Timeout') timers++; } });
  const pending = [];
  hook.enable();
  try {
    for (let index = 0; index < 64; index++) pending.push(registry.register(1000));
  } finally {
    hook.disable();
    registry.close();
    await Promise.all(pending.map(operation => assert.rejects(operation.promise, OperationCancelledError)));
  }
  assert.equal(timers, 0);
  assert.deepEqual(transitions, [1, 0], 'a batch reports only empty/nonempty transitions');
});

test('deadline maintenance yields its live iterator and resumes through deletion', async () => {
  let now = 0;
  const registry = new OperationRegistry({
    now: () => now++
  });
  const pending = Array.from({ length: 4 }, () => registry.register(0));
  const terminal = pending.map(operation => operation.promise.catch(error => error));
  const completed = registry.expire(100, now + 2);
  assert.ok(completed > 0 && completed < pending.length);
  assert.equal(registry.complete(pending[2].id, 'reply'), true);
  registry.expire(100, now + 20);
  assert.equal(await terminal[2], 'reply');
  for (const index of [0, 1, 3]) assert.ok((await terminal[index]) instanceof OperationTimeoutError);
  assert.equal(registry.complete(pending[3].id, 'late'), false);
  const later = registry.register(0);
  const laterTerminal = later.promise.catch(error => error);
  registry.expire(100, now + 20);
  assert.ok((await laterTerminal) instanceof OperationTimeoutError, 'an exhausted iterator must observe later registrations');
  registry.close();
});


test('shared deadline maintenance completes a native request while its handler is suspended', async () => {
  const backend = require('../../packages/framework/dist/runtime/backend');
  const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const suffix = `node-dispatch-deadline-${process.pid}`;
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 2n));
  const node = factory.createMeshAdapter().createMeshNode(context, { meshName: suffix, routingId: suffix, applicationJobQueue: queue });
  node.setBind(`inproc://${suffix}`);
  node.start();
  const table = new backend.ZLinkMeshCompletionTable();
  let entered;
  const started = new Promise(resolve => { entered = resolve; });
  let release;
  const blocked = new Promise(resolve => { release = resolve; });
  let late;
  const replied = new Promise(resolve => { late = resolve; });
  let completions = 0;
  const pump = new backend.ZLinkMeshDispatchPump(node, {
    applicationJobQueue: queue,
    async dispatch(_owner, record) {
      if (record.kind === require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts').ReceiveKind.Completion) { completions++; table.complete(record); return; }
      entered();
      await blocked;
      await record.reply(Buffer.from('late'));
      late();
    }
  });
  pump.start();
  try {
    const pending = table.submit(() => node.requestToNode(node.status().routingId, Buffer.from('request'), { timeoutMs: 20 }));
    await started;
    const completion = await pending;
    assert.equal(completion.terminalResult, RequestResult.TimedOut);
    backend.closeMeshCompletion(completion);
    assert.equal(table.pendingCount, 0);
    release();
    await replied;
    await new Promise(setImmediate);
    assert.equal(completions, 1, 'late native reply cannot establish another terminal');
  } finally {
    release();
    table.dispose();
    await pump.dispose();
    node.close();
    await context.dispose();
  }
});

test('shared maintenance budget advances both registries while raw work carries over', async () => {
  let now = 0;
  const clock = { now: () => now++ };
  const raw = new OperationRegistry(clock);
  const stateful = new OperationRegistry(clock);
  const rawPending = Array.from({ length: 8 }, () => raw.register(0));
  const statefulPending = Array.from({ length: 4 }, () => stateful.register(0));
  const rawTerminals = rawPending.map(operation => operation.promise.catch(error => error));
  const statefulTerminals = statefulPending.map(operation => operation.promise.catch(error => error));
  let rawExpired = 0;
  let statefulExpired = 0;
  for (let turn = 0; turn < 8; turn++) {
    const start = now;
    rawExpired += raw.expire(1000, start + 2);
    statefulExpired += stateful.expire(1000, start + 4);
    if (turn === 0) {
      assert.ok(rawExpired > 0 && rawExpired < rawPending.length);
      assert.ok(statefulExpired > 0, 'raw carry-over cannot starve stateful maintenance');
    }
  }
  for (const result of await Promise.all([...rawTerminals, ...statefulTerminals])) assert.ok(result instanceof OperationTimeoutError);
  raw.close();
  stateful.close();
});


test('backend shared maintenance gives raw and stateful registries one divided budget', async t => {
  const backend = require('../../packages/framework/dist/runtime/backend');
  const { RAW_MESH_RECEIVE_TIME_BUDGET_MS } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
  const observations = [];
  let completed;
  const observed = new Promise(resolve => { completed = resolve; });
  const expire = OperationRegistry.prototype.expire;
  t.mock.method(OperationRegistry.prototype, 'expire', function(now, deadline) {
    observations.push({ now, deadline });
    if (observations.length === 2) completed();
    return expire.call(this, now, deadline);
  });
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const suffix = `node-dispatch-budget-${process.pid}`;
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}, () => 2n));
  const node = factory.createMeshAdapter().createMeshNode(context, { meshName: suffix, routingId: suffix, applicationJobQueue: queue });
  node.setBind(`inproc://${suffix}`);
  try {
    node.start();
    await observed;
    await new Promise(setImmediate);
    assert.equal(observations[0].now, observations[1].now);
    assert.equal(observations[0].deadline - observations[0].now, RAW_MESH_RECEIVE_TIME_BUDGET_MS / 2);
    assert.equal(observations[1].deadline - observations[1].now, RAW_MESH_RECEIVE_TIME_BUDGET_MS);
  } finally {
    node.close();
    await context.dispose();
  }
});
