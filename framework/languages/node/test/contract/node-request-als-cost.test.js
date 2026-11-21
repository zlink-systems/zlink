'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { AsyncLocalStorage, createHook } = require('node:async_hooks');
const {
  FrameworkContextField
} = require('../../packages/framework/dist/runtime/framework-context');

// Module ownership remains meaningful if individual helper functions are renamed.
const FLOW_MODULE = '/runtime/diagnostics/flow-context.js:';
const PERMIT_MODULE = '/runtime/application-jobs/application-job-queue-scope.js:';
const REQUEST_COUNT = 40;

function assertOff(row) {
  assert.equal(row.flowRun, 0, 'Off must not install a diagnostics ALS scope');
  assert.equal(row.flowEnter, 0, 'Off must not enter a diagnostics ALS scope');
  assert.equal(row.flowResources, 0, 'Off must not propagate diagnostics ALS to resources');
}

function observeAls() {
  const originals = [AsyncLocalStorage.prototype, FrameworkContextField.prototype].map(
    (prototype) => ({
      prototype,
      run: prototype.run,
      enterWith: prototype.enterWith,
      restore: prototype.restore
    })
  );
  const originalPropagate = AsyncLocalStorage.prototype._propagate;
  const flowStores = new Set();
  const frameworkStores = new Set();
  const promisePropagates = new Map();
  let active;
  let inspecting = false;

  function wrap(original, operation, native) {
    return function observed(...args) {
      if (!inspecting) {
        inspecting = true;
        try {
          const stack = new Error().stack;
          // The immediate caller owns the ALS operation; ignore upstream callers.
          const owner = stack.split('\n')[2].replaceAll('\\', '/');
          if (native && owner.includes('/framework/dist/runtime/')) frameworkStores.add(this);
          if (owner.includes(FLOW_MODULE)) {
            flowStores.add(this);
            if (active) active[operation === 'run' ? 'flowRun' : 'flowEnter']++;
          }
          if (active && owner.includes(PERMIT_MODULE)) {
            active[operation === 'restore' ? 'permitRestore' : 'permitEnter']++;
          }
        } finally {
          inspecting = false;
        }
      }
      return Reflect.apply(original, this, args);
    };
  }

  for (const original of originals) {
    original.prototype.run = wrap(
      original.run,
      'run',
      original.prototype === AsyncLocalStorage.prototype
    );
    original.prototype.enterWith = wrap(
      original.enterWith,
      'enterWith',
      original.prototype === AsyncLocalStorage.prototype
    );
    if (original.restore !== undefined) {
      original.prototype.restore = wrap(original.restore, 'restore', false);
    }
  }
  AsyncLocalStorage.prototype._propagate = function (resource, trigger, type) {
    if (active && type === 'PROMISE' && frameworkStores.has(this)) {
      promisePropagates.set(resource, (promisePropagates.get(resource) ?? 0) + 1);
    }
    return originalPropagate.call(this, resource, trigger, type);
  };
  const hook = createHook({
    init(_id, type) {
      if (!active || inspecting) return;
      inspecting = true;
      try {
        active.resources[type] = (active.resources[type] ?? 0) + 1;
        for (const storage of flowStores) {
          if (storage.getStore() !== undefined) {
            active.flowResources++;
            break;
          }
        }
      } finally {
        inspecting = false;
      }
    }
  });
  hook.enable();
  return {
    start() {
      promisePropagates.clear();
      active = { flowRun: 0, flowEnter: 0, permitEnter: 0, permitRestore: 0, flowResources: 0, resources: {} };
      return active;
    },
    stop() {
      if (active) {
        active.frameworkPromiseResources = promisePropagates.size;
        active.frameworkPropagatesPerPromise = [...new Set(promisePropagates.values())];
      }
      active = undefined;
      promisePropagates.clear();
    },
    restore() {
      active = undefined;
      hook.disable();
      for (const original of originals) {
        original.prototype.run = original.run;
        original.prototype.enterWith = original.enterWith;
        if (original.restore !== undefined) original.prototype.restore = original.restore;
      }
      AsyncLocalStorage.prototype._propagate = originalPropagate;
    }
  };
}

test('public requests skip diagnostics ALS at Off and preserve flow at Normal', async (t) => {
  const events = [];
  const observation = require('./fixtures/message-flow-observation.cjs')(
    require('../../packages/framework/dist/runtime/diagnostics/message-flow-delivery'),
    {
      onLog(record) {
        events.push(record.attributes);
      }
    }
  );
  await observation.observed();
  const observer = observeAls();
  const rows = [];
  try {
    // Install wrappers and the public OTel observer before loading the runtime.
    const { frameworkPair } = require('../../perf/request-backpressure.cjs');
    for (const mode of ['off', 'normal']) {
      const pair = await frameworkPair(mode);
      try {
        pair.validate(await pair.submit().reply);
        await observation.observed();
        events.length = 0;
        const row = observer.start();
        let completed = 0;
        for (let index = 0; index < REQUEST_COUNT; index++) {
          pair.validate(await pair.submit().reply);
          completed++;
        }
        observer.stop();
        await observation.observed();
        assert.equal(completed, REQUEST_COUNT);
        assert.equal(row.permitEnter, REQUEST_COUNT, 'record permit is entered once');
        assert.equal(row.permitRestore, REQUEST_COUNT, 'record permit is restored once');
        assert.ok(row.resources.PROMISE > 0, 'async_hooks observes actual request resources');
        t.diagnostic(JSON.stringify({ mode, completed, ...row }));
        rows.push(row);
        if (mode === 'off') {
          assertOff(row);
          assert.equal(events.length, 0, 'Off emits no message-flow telemetry');
        } else {
          assert.ok(row.flowRun > 0, 'same wrapper observes enabled diagnostics ALS');
          assert.ok(row.flowResources > 0, 'async_hooks sees actual flow propagation');
          assert.throws(() => assertOff(row), { code: 'ERR_ASSERTION' });
          t.diagnostic('negative control: Off checker rejected actual Normal observations');
          const requests = new Map();
          for (const event of events) {
            if (event.event_id !== 'zlink.message_flow' || !event.correlation_id) continue;
            assert.ok(event.flow_id, 'request event retains its flow id');
            assert.ok(event.flow_origin, 'request event retains its flow origin');
            const existing = requests.get(event.correlation_id);
            const flow = [event.flow_id, event.flow_origin];
            if (existing)
              assert.deepEqual(flow, existing.flow, 'request/reply correlation keeps one flow');
            else requests.set(event.correlation_id, { flow, phases: new Set() });
            requests.get(event.correlation_id).phases.add(event.phase);
          }
          assert.equal(requests.size, REQUEST_COUNT, 'each completed request has a correlation id');
          for (const { phases } of requests.values()) {
            assert.ok(phases.has('received'), 'correlation includes request receipt');
            assert.ok(phases.has('reply_received'), 'correlation includes terminal reply receipt');
          }
        }
      } finally {
        observer.stop();
        await pair.close();
      }
    }
    for (const row of rows) {
      assert.ok(
        row.frameworkPromiseResources > 0,
        'actual requests create observed Framework promises'
      );
      assert.equal(
        row.frameworkPromiseResources,
        row.resources.PROMISE,
        'every request promise is counted'
      );
      assert.deepEqual(
        row.frameworkPropagatesPerPromise,
        [1],
        'one Framework propagation per request promise'
      );
    }
  } finally {
    observer.restore();
    observation.close();
  }
});
