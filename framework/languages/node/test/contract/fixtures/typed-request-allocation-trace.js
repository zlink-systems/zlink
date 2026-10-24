'use strict';
const assert = require('node:assert/strict');
const { Session } = require('node:inspector');
const { frameworkPair } = require('../../../perf/request-backpressure.cjs');

// A function allocated for each completed request verifies that tracking records
// allocations even when the returned object is no longer reachable at snapshot.
function allocationControl() {
  return () => undefined;
}

const OWNER_MODULES = [
  '/runtime/backend/mesh-completion-table.js',
  '/runtime/backend/node/node-raw-binding-port.js',
  '/runtime/messaging/request-result.js',
  '/runtime/foundation/operation-registry.js'
];
async function trace() {
  const count = 100;
  const pair = await frameworkPair();
  const session = new Session();
  session.connect();
  const post = (method, params = {}) =>
    new Promise((resolve, reject) => {
      session.post(method, params, (error, result) => (error ? reject(error) : resolve(result)));
    });
  const chunks = [];
  try {
    for (let i = 0; i < 20; i++) {
      const request = pair.submit();
      pair.validate(await request.reply);
      allocationControl();
    }
    global.gc();
    await post('HeapProfiler.startTrackingHeapObjects', { trackAllocations: true });
    for (let i = 0; i < count; i++) {
      const request = pair.submit();
      pair.validate(await request.reply);
      allocationControl();
    }
    session.on('HeapProfiler.addHeapSnapshotChunk', ({ params }) => chunks.push(params.chunk));
    await post('HeapProfiler.stopTrackingHeapObjects', { reportProgress: false });
    const snapshot = JSON.parse(chunks.join(''));
    const infoFields = snapshot.snapshot.meta.trace_function_info_fields;
    const fields = snapshot.snapshot.meta.trace_node_fields;
    const infos = snapshot.trace_function_infos;
    const modules = Object.fromEntries(
      OWNER_MODULES.map((name) => [name, { objects: 0, bytes: 0 }])
    );
    const control = { objects: 0, bytes: 0 };
    const maintenance = { objects: 0, bytes: 0 };
    function visit(nodes) {
      for (let index = 0; index < nodes.length; index += fields.length) {
        const infoIndex = nodes[index + fields.indexOf('function_info_index')] * infoFields.length;
        const script = snapshot.strings[infos[infoIndex + infoFields.indexOf('script_name')]];
        const name = snapshot.strings[infos[infoIndex + infoFields.indexOf('name')]];
        const objects = nodes[index + fields.indexOf('count')];
        const bytes = nodes[index + fields.indexOf('size')];
        const owner = OWNER_MODULES.find((module) => script.replaceAll('\\', '/').endsWith(module));
        // Periodic deadline traversal runs independently of the request count.
        // Keep its actual allocations visible, outside the remote request budget.
        const periodic = owner === '/runtime/foundation/operation-registry.js' &&
          (name === 'expire' || name === 'nextDeadlineMs');
        if (periodic) { maintenance.objects += objects; maintenance.bytes += bytes; }
        else if (owner) {
          modules[owner].objects += objects;
          modules[owner].bytes += bytes;
        }
        if (script === __filename && name === 'allocationControl') {
          control.objects += objects;
          control.bytes += bytes;
        }
        visit(nodes[index + fields.indexOf('children')]);
      }
    }
    visit(snapshot.trace_tree);
    assert.ok(control.objects >= count, `V8 allocation control: ${control.objects} < ${count}`);
    assert.ok(control.bytes > 0, 'V8 tracks control bytes');
    const total = Object.values(modules).reduce(
      (sum, row) => ({
        objects: sum.objects + row.objects,
        bytes: sum.bytes + row.bytes
      }),
      { objects: 0, bytes: 0 }
    );
    assert.ok(total.objects > count, 'request owner modules have real allocations');
    return { node: process.version, count, control, maintenance, modules, total };
  } finally {
    session.disconnect();
    await pair.close();
  }
}
trace().then(
  (result) => process.stdout.write(`${JSON.stringify(result)}\n`),
  (error) => {
    console.error(error);
    process.exitCode = 1;
  }
);
