'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { setImmediate: nextTurn } = require('node:timers/promises');
const { ZLinkNodeRawMeshBackend } =
  require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { ZLinkNodeRawBindingPort } =
  require('../../packages/framework/dist/runtime/backend/node/node-raw-binding-port');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } =
  require('../../packages/framework/dist/runtime/host/application-job-queue');

function fixture(registrationFailure) {
  const queue = new ApplicationJobQueue(resolveApplicationJobQueueConfiguration(
    { maxQueuedApplicationJobs: 1n }, () => 1n
  ));
  let readable;
  let pending = 0;
  let receives = 0;
  let released = 0;
  let registrations = 0;
  let monitorDrains = 0;
  let closed = false;
  let drained;
  let monitorDrained;
  const sourceRoute = Buffer.from('peer');
  const parts = [Buffer.from([0xff])];
  const router = {
    setRoutingId() {}, setReceiveFlowState() {}, bind() {},
    localEndpoint: () => 'tcp://127.0.0.1:54321',
    setReadableHandler(handler) {
      registrations++;
      if (registrationFailure) throw registrationFailure;
      readable = handler;
    },
    monitor: () => ({ drain() { monitorDrains++; monitorDrained?.(); return 0; }, close() {} }),
    receive(dontWait) {
      assert.equal(closed, false);
      assert.equal(typeof readable, 'function', 'ingress requires binding readiness registration');
      assert.equal(dontWait, true);
      assert.equal(queue.snapshot().permitsInUse, 1n, 'receive must own the host permit');
      receives++;
      if (pending === 0) { drained?.(); return undefined; }
      pending--;
      return { sourceRid: 'peer', sourceRoute, parts, close() { released++; } };
    },
    close() { closed = true; }
  };
  const host = { createRouter: () => router, close() { router.close(); } };
  const backend = new ZLinkNodeRawMeshBackend('readable', 'readable-node',
    { createHost: () => host }, queue);
  backend.setBind('tcp://127.0.0.1:54321');
  return {
    backend, queue,
    get state() { return { receives, released, registrations, monitorDrains, closed }; },
    notify(count) {
      pending += count;
      readable();
    },
    untilDrained() { return new Promise(resolve => { drained = resolve; }); },
    untilMonitorDrained() { return new Promise(resolve => { monitorDrained = resolve; }); }
  };
}

test('RouteMesh registers readiness at startup and idle maintenance never receives', async () => {
  const f = fixture();
  f.backend.start();
  try {
    assert.equal(f.state.registrations, 1);
    await f.untilMonitorDrained();
    await f.untilMonitorDrained();
    assert.ok(f.state.monitorDrains > 1, 'idle monitor work must still progress');
    assert.equal(f.state.receives, 0);
  } finally { f.backend.close(); }
});

test('one readiness notification drains beyond a batch to no-data with the host permit', async () => {
  const f = fixture();
  f.backend.start();
  try {
    const drained = f.untilDrained();
    f.notify(130);
    await drained;
    await nextTurn();
    assert.equal(f.state.released, 130);
    assert.ok(f.state.receives >= 131, 'the receive result, not the notification, ends the drain');
    assert.equal(f.queue.snapshot().permitsInUse, 0n);
    const receives = f.state.receives;
    await f.untilMonitorDrained();
    await f.untilMonitorDrained();
    assert.equal(f.state.receives, receives, 'no-data ends receive work until another notification');
  } finally { f.backend.close(); }
});

test('readiness during a capacity wait coalesces into one drain and resumes without another event', async () => {
  const f = fixture();
  const occupied = await f.queue.acquire();
  f.backend.start();
  try {
    const drained = f.untilDrained();
    f.notify(1);
    await nextTurn();
    for (let index = 0; index < 10; index++) f.notify(1);
    await nextTurn();
    assert.equal(f.state.receives, 0);
    assert.equal(f.queue.snapshot().capacityWaiters, 1n);
    occupied.releaseAfterInternalProcessing();
    await drained;
    await nextTurn();
    assert.equal(f.state.released, 11);
    assert.equal(f.queue.snapshot().permitsInUse, 0n);
  } finally { f.backend.close(); }
});

for (const handoff of [false, true]) test(`close ends readiness admission across permit handoff=${handoff}`, async () => {
  const f = fixture();
  const occupied = await f.queue.acquire();
  f.backend.start();
  f.notify(1);
  await nextTurn();
  assert.equal(f.queue.snapshot().capacityWaiters, 1n);
  if (handoff) occupied.releaseAfterInternalProcessing();
  f.backend.close();
  f.notify(1);
  await nextTurn();
  assert.equal(f.state.receives, 0);
  assert.equal(f.queue.snapshot().capacityWaiters, 0n);
  if (!handoff) occupied.releaseAfterInternalProcessing();
  assert.equal(f.queue.snapshot().permitsInUse, 0n);
});

test('RouteMesh startup fails and closes its socket if readiness registration fails', () => {
  const failure = new Error('watch registration failed');
  const f = fixture(failure);
  assert.throws(() => f.backend.start(), error => error === failure);
  assert.equal(f.state.closed, true);
  assert.equal(f.state.receives, 0);
});

test('raw binding port delegates registration to the public socket handler', t => {
  const host = new ZLinkNodeRawBindingPort().createHost();
  try {
    const router = host.createRouter();
    const registered = t.mock.method(router.socket, 'setReadableHandler');
    const handler = () => {};
    router.setReadableHandler(handler);
    assert.equal(registered.mock.callCount(), 1);
    assert.equal(registered.mock.calls[0].arguments[0], handler);
  } finally { host.close(); }
});
