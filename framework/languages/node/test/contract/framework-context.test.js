const assert = require('node:assert/strict');
const { AsyncLocalStorage } = require('node:async_hooks');
const test = require('node:test');

const {
  FrameworkContextField
} = require('../../packages/framework/dist/runtime/framework-context');
const actor = require('../../packages/framework/dist/runtime/actors/actor-execution-context');
const join = require('../../packages/framework/dist/runtime/actors/actor-join-deferred-scope');
const permit = require('../../packages/framework/dist/runtime/application-jobs/application-job-queue-scope');
const flow = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
const execution = require('../../packages/framework/dist/runtime/execution');
const {
  ZLinkSerialExecutionQueue
} = require('../../packages/framework/dist/runtime/execution/serial-execution-queue');
const {
  ZLinkStateLane,
  startOutsideStateLane
} = require('../../packages/framework/dist/runtime/execution/state-lane');

test('eleven Framework fields have one promise propagation owner', async () => {
  const fields = Array.from({ length: 11 }, () => new FrameworkContextField());
  const carriers = new Set();
  const counts = new Map();
  const originalRun = AsyncLocalStorage.prototype.run;
  const originalPropagate = AsyncLocalStorage.prototype._propagate;
  AsyncLocalStorage.prototype.run = function (...args) {
    carriers.add(this);
    return originalRun.apply(this, args);
  };
  try {
    for (const field of fields) field.run('active', () => {});
    assert.equal(carriers.size, 1);
    AsyncLocalStorage.prototype._propagate = function (resource, trigger, type) {
      if (carriers.has(this) && type === 'PROMISE') {
        counts.set(resource, (counts.get(resource) ?? 0) + 1);
      }
      return originalPropagate.call(this, resource, trigger, type);
    };
    for (let index = 0; index < 40; index++) await Promise.resolve();
    assert.ok(counts.size >= 80);
    assert.ok([...counts.values()].every((count) => count === 1));
  } finally {
    AsyncLocalStorage.prototype.run = originalRun;
    AsyncLocalStorage.prototype._propagate = originalPropagate;
  }
});

test('field entries allocate zero or one immutable carrier and preserve siblings', () => {
  const actor = new FrameworkContextField();
  const flow = new FrameworkContextField();
  const stores = [];
  let copies = 0;
  const originalRun = AsyncLocalStorage.prototype.run;
  const originalFreeze = Object.freeze;
  Object.freeze = function (value) {
    if (Object.getOwnPropertySymbols(value).length > 0) copies++;
    return originalFreeze(value);
  };
  AsyncLocalStorage.prototype.run = function (store, ...args) {
    stores.push(store);
    return originalRun.call(this, store, ...args);
  };
  try {
    actor.run('A', () => {
      const parent = stores.at(-1);
      actor.run('A', () => assert.equal(stores.at(-1), parent));
      flow.run('F', () => {
        const child = stores.at(-1);
        assert.notEqual(child, parent);
        assert.equal(actor.getStore(), 'A');
        flow.run('F', () => assert.equal(stores.at(-1), child));
        actor.exit(() => {
          assert.equal(actor.getStore(), undefined);
          assert.equal(flow.getStore(), 'F');
          const detached = stores.at(-1);
          assert.notEqual(detached, child);
          actor.exit(() => assert.equal(stores.at(-1), detached));
        });
        assert.equal(actor.getStore(), 'A');
      });
      assert.equal(flow.getStore(), undefined);
    });
    assert.equal(new Set(stores).size, 3);
    assert.equal(copies, 3);
    assert.ok(stores.every(Object.isFrozen));
    assert.equal(actor.getStore(), undefined);
    assert.equal(flow.getStore(), undefined);
  } finally {
    AsyncLocalStorage.prototype.run = originalRun;
    Object.freeze = originalFreeze;
  }
});

test('copy on write isolates sibling continuations across run, exit and enterWith', async () => {
  const actor = new FrameworkContextField();
  const flow = new FrameworkContextField();
  for (let index = 0; index < 40; index++) {
    const rows = await actor.run('A', () =>
      flow.run('F', () =>
        Promise.all([
          actor.run('B', async () => {
            await Promise.resolve();
            return [actor.getStore(), flow.getStore()];
          }),
          flow.run('G', async () => {
            await Promise.resolve();
            return [actor.getStore(), flow.getStore()];
          }),
          actor.exit(async () => {
            await Promise.resolve();
            return [actor.getStore(), flow.getStore()];
          }),
          actor.run('C', () => {
            const previous = actor.getStore();
            actor.enterWith('D');
            const continuation = Promise.resolve().then(() => [actor.getStore(), flow.getStore()]);
            flow.enterWith('H');
            actor.enterWith(previous);
            assert.equal(flow.getStore(), 'H');
            assert.equal(actor.getStore(), 'C');
            return continuation;
          })
        ])
      )
    );
    assert.deepEqual(rows, [
      ['B', 'F'],
      ['A', 'G'],
      [undefined, 'F'],
      ['D', 'F']
    ]);
    assert.equal(actor.getStore(), undefined);
    assert.equal(flow.getStore(), undefined);
  }
});

test('Actor and lane detach clear only their owned fields across await', async () => {
  const lane = new ZLinkStateLane();
  const reservation = {
    origin: 'inbound',
    detachedForHandlerTurn: false,
    releaseAfterInternalProcessing() {}
  };
  const currentFlow = { flowId: 'flow', flowOrigin: 'Inbound' };
  for (let index = 0; index < 40; index++) {
    await lane.run(() =>
      permit.runWithApplicationJobPermit(reservation, () =>
        flow.runWithFlow(currentFlow, () =>
          execution.runZLinkExecutionArea('application', () =>
            actor.runZLinkActorExecution('actor', 'spot', async () => {
              await actor.runOutsideZLinkActorExecution(async () => {
                await Promise.resolve();
                assert.equal(actor.currentZLinkActorExecution(), undefined);
                assert.equal(ZLinkStateLane.current, lane);
                assert.equal(permit.currentApplicationJobPermit(), reservation);
                assert.equal(flow.currentFlowContext(), currentFlow);
                assert.equal(execution.currentZLinkExecutionArea(), 'application');
              });
              await startOutsideStateLane(async () => {
                await Promise.resolve();
                assert.equal(ZLinkStateLane.current, undefined);
                assert.equal(actor.currentZLinkActorExecution().actorId, 'actor');
                assert.equal(permit.currentApplicationJobPermit(), reservation);
                assert.equal(flow.currentFlowContext(), currentFlow);
                assert.equal(execution.currentZLinkExecutionArea(), 'application');
              });
              assert.equal(ZLinkStateLane.current, lane);
              assert.equal(actor.currentZLinkActorExecution().actorId, 'actor');
            })
          )
        )
      )
    );
    assert.equal(actor.currentZLinkActorExecution(), undefined);
    assert.equal(permit.currentApplicationJobPermit(), undefined);
    assert.equal(flow.currentFlowContext(), undefined);
    assert.equal(execution.currentZLinkExecutionArea(), undefined);
  }
});

test('an inherited Actor claim remains inactive after its handler terminal', async () => {
  let release;
  let detached;
  await actor.runZLinkActorExecution('actor', 'spot', () => {
    detached = new Promise((resolve) => {
      release = resolve;
    }).then(() => actor.currentZLinkActorExecution());
  });
  release();
  assert.equal(await detached, undefined);
});

test('an inherited Join scope cannot reopen after its handler terminal', async () => {
  for (let index = 0; index < 40; index++) {
    let release;
    let detached;
    let discarded = 0;
    await join.runActorHandlerWithDeferredJoins(() => {
      detached = new Promise((resolve) => {
        release = resolve;
      }).then(() => {
        assert.throws(
          () =>
            join.deferActorJoin({
              async execute() {
                assert.fail('a sealed Join scope must not admit work');
              },
              discard() {
                discarded++;
              }
            }),
          /open Framework actor handler scope/u
        );
      });
    });
    release();
    await detached;
    assert.equal(discarded, 1);
  }
});

test('disabled flow tracing never enters the carrier', () => {
  const originalRun = AsyncLocalStorage.prototype.run;
  let entries = 0;
  AsyncLocalStorage.prototype.run = function (...args) {
    entries++;
    return originalRun.apply(this, args);
  };
  try {
    assert.equal(
      flow.runWithOutboundFlow(false, () => flow.runWithFlow(undefined, () => 42)),
      42
    );
    assert.equal(flow.currentFlowContext(), undefined);
    assert.equal(entries, 0);
  } finally {
    AsyncLocalStorage.prototype.run = originalRun;
  }
});

test('serial work carries its own flow and only its explicitly admitted permit', async () => {
  const queue = new ZLinkSerialExecutionQueue(async (record) => {
    try {
      record.resolve(await record.operation());
    } catch (error) {
      record.reject(error);
    } finally {
      record.release();
    }
  });
  const firstFlow = { flowId: 'first', flowOrigin: 'Inbound' };
  const secondFlow = { flowId: 'second', flowOrigin: 'Inbound' };
  const reservation = {
    origin: 'inbound',
    detachedForHandlerTurn: false,
    releaseAfterInternalProcessing() {}
  };
  const readContext = () => ({
    flow: flow.currentFlowContext(),
    permit: permit.currentApplicationJobPermit(),
    area: execution.currentZLinkExecutionArea()
  });
  const first = execution.runZLinkExecutionArea('application', () =>
    permit.runWithApplicationJobPermit(reservation, () =>
      flow.runWithFlow(firstFlow, () => queue.submitPreAdmitted(readContext))
    )
  );
  const second = flow.runWithFlow(secondFlow, () => queue.submit(readContext));
  const third = queue.submit(readContext);
  assert.deepEqual(await Promise.all([first, second, third]), [
    { flow: firstFlow, permit: reservation, area: undefined },
    { flow: secondFlow, permit: undefined, area: undefined },
    { flow: undefined, permit: undefined, area: undefined }
  ]);
});

test('leaving a job permit restores the existing carrier without allocating a clear value', (t) => {
  const sibling = new FrameworkContextField();
  const reservation = {};
  const originalFreeze = Object.freeze;
  let copies = 0;
  t.mock.method(Object, 'freeze', (value) => {
    if (Object.getOwnPropertySymbols(value).length !== 0) copies++;
    return originalFreeze(value);
  });
  sibling.run('parent', () => {
    const previous = permit.enterApplicationJobPermitScope(reservation);
    assert.equal(permit.currentApplicationJobPermit(), reservation);
    const copiesAtEntry = copies;
    permit.leaveApplicationJobPermitScope(previous);
    assert.equal(copies, copiesAtEntry);
    assert.equal(permit.currentApplicationJobPermit(), undefined);
    assert.equal(sibling.getStore(), 'parent');
  });
});
