const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkRequestResult } = require('../../packages/framework/dist/runtime/messaging/request-result');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { OperationRegistry } = require('../../packages/framework/dist/runtime/foundation/operation-registry');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');

// Count slot visits rather than timings: pending table size must not affect
// maintenance scheduling on either request submission path (spec 08 E3).
for (const pendingCount of [64, 1024]) {
  for (const method of ['requestToNode', 'requestToChannel']) {
    test(`${method} schedules without scanning ${pendingCount} pending completion slots`, () => {
      const table = new ZLinkMeshCompletionTable();
      for (let id = 1; id <= pendingCount; id++)
        void table.submit(() => ({ high: 1n, low: BigInt(id) })).pending.catch(() => {});
      let visits = 0;
      table.slots = new Proxy(table.slots, {
        get(target, key, receiver) {
          if (typeof key === 'string' && /^\d+$/.test(key)) visits++;
          return Reflect.get(target, key, receiver);
        }
      });
      const backend = new ZLinkNodeRawMeshBackend('deadline-test', 'source', {});
      backend.setCompletionTable(table);
      const registry = new OperationRegistry(undefined, () => backend.scheduleMaintenance());
      backend.runtime = {
        [method]() {
          return { id: registry.registerObserved(1000, { operationCompleted() {}, operationFailed() {} }) };
        },
        nextMaintenanceDeadlineMs: () => registry.nextDeadlineMs(),
        get pendingOperationCount() { return registry.size; }
      };
      try {
        backend[method]('target', Buffer.from('request'), { requestEntry: new ZLinkRequestResult() });
        assert.equal(visits, 0, 'request registration must not inspect completion slots');
      } finally {
        registry.close();
        clearTimeout(backend.maintenanceTimer);
        table.dispose();
      }
    });
  }
}
