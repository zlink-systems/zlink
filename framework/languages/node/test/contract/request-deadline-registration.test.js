const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkRequestResult } = require('../../packages/framework/dist/runtime/messaging/request-result');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require('../../packages/framework/dist/runtime/host/application-job-queue');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');

// Count slot visits rather than timings: pending table size must not affect
// maintenance scheduling on either request submission path (spec 08 E3).
for (const pendingCount of [64, 1024]) {
  for (const method of ['requestToNode', 'requestToChannel']) {
    test(`${method} schedules without scanning ${pendingCount} pending completion slots`, async (t) => {
      let visits = 0;
      const table = new ZLinkMeshCompletionTable(undefined, { onSlotVisit() { visits++; } });
      for (let id = 1; id <= pendingCount; id++)
        void table.submit(() => ({ high: 1n, low: BigInt(id) })).pending.catch(() => {});
      assert.equal(table.pendingCount, pendingCount);
      assert.ok(visits > 0, 'slot instrumentation observes table population');
      const router = {
        setRoutingId() {}, setReceiveFlowState() {}, setReadableHandler() {},
        bind() {}, close() {}, receive() {}, routesSnapshot: () => [],
        localEndpoint: () => 'inproc://deadline-test'
      };
      const bindingPort = {
        createHost: () => ({ createRouter: () => router, shutdown() {}, close() {} })
      };
      const backend = new ZLinkNodeRawMeshBackend(
        'deadline-test', 'source', bindingPort,
        new ApplicationJobQueue(resolveApplicationJobQueueConfiguration())
      );
      backend.setCompletionTable(table);
      backend.setBind('inproc://deadline-test');
      const timers = t.mock.method(globalThis, 'setTimeout');
      try {
        backend.start();
        const scheduledBefore = timers.mock.callCount();
        const requestEntry = new ZLinkRequestResult();
        void requestEntry.pending.catch(() => {});
        visits = 0;
        backend[method]('target', Buffer.from('request'), { timeoutMs: 1000, requestEntry });
        assert.ok(timers.mock.callCount() > scheduledBefore, 'request registration arms maintenance');
        assert.equal(visits, 0, 'request registration must not inspect completion slots');
      } finally {
        await backend.close();
      }
    });
  }
}
