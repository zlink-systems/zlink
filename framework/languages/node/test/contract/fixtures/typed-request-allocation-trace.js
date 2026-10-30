'use strict';
const assert = require('node:assert/strict');
const { Session } = require('node:inspector');
const {
  OperationRegistry
} = require('../../../packages/framework/dist/runtime/foundation/operation-registry');

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
async function trace({ registryOnly = false } = {}) {
  assert.equal(typeof global.gc, 'function', 'Run allocation tracing with --expose-gc');
  const count = 100;
  // Materialize a fresh fractional clock value on every call, as the system
  // clock does. Integer wall-clock samples can omit a HeapNumber; a constant
  // fractional value reuses its box instead of measuring that required cost.
  let clockTick = 0;
  function calibrationClockNow() {
    return ++clockTick + 0.5;
  }
  const registrationRegistry = new OperationRegistry({ now: calibrationClockNow });
  const observer = { operationCompleted() {}, operationFailed() {} };
  function requiredRegistrationControl() {
    const id = registrationRegistry.registerObserved(30000, observer, 1);
    registrationRegistry.complete(id, undefined);
  }
  let nextControlId = 1n;
  function identityControl() {
    return nextControlId++;
  }
  const requestRegistry = new OperationRegistry();
  const pair = registryOnly
    ? {
        submit() {
          const id = requestRegistry.registerObserved(30000, observer, 1);
          requestRegistry.complete(id, undefined);
          return { reply: Promise.resolve() };
        },
        validate() {},
        async close() {
          requestRegistry.close();
        }
      }
    : await require('../../../perf/request-backpressure.cjs').frameworkPair();
  const session = new Session();
  session.connect();
  const post = (method, params = {}) =>
    new Promise((resolve, reject) => {
      session.post(method, params, (error, result) => (error ? reject(error) : resolve(result)));
    });
  try {
    for (let i = 0; i < count; i++) {
      const request = pair.submit();
      pair.validate(await request.reply);
      allocationControl();
      requiredRegistrationControl();
      identityControl();
    }
    const measure = async () => {
      // Each window starts after GC, and measures the request and its controls
      // together. Trace counts include transient allocations, not only survivors.
      global.gc();
      const chunks = [];
      const receiveChunk = ({ params }) => chunks.push(params.chunk);
      session.on('HeapProfiler.addHeapSnapshotChunk', receiveChunk);
      await post('HeapProfiler.startTrackingHeapObjects', { trackAllocations: true });
      for (let i = 0; i < count; i++) {
        const request = pair.submit();
        pair.validate(await request.reply);
        allocationControl();
        requiredRegistrationControl();
        identityControl();
      }
      await post('HeapProfiler.stopTrackingHeapObjects', { reportProgress: false });
      session.off('HeapProfiler.addHeapSnapshotChunk', receiveChunk);
      const snapshot = JSON.parse(chunks.join(''));
      const infoFields = snapshot.snapshot.meta.trace_function_info_fields;
      const fields = snapshot.snapshot.meta.trace_node_fields;
      const infos = snapshot.trace_function_infos;
      const modules = Object.fromEntries(
        OWNER_MODULES.map((name) => [name, { objects: 0, bytes: 0 }])
      );
      const control = { objects: 0, bytes: 0 };
      const maintenance = { objects: 0, bytes: 0 };
      const requiredRegistration = { objects: 0, bytes: 0 };
      const registrationIndex = { objects: 0, bytes: 0 };
      const identity = { objects: 0, bytes: 0 };
      const sites = [];
      function visit(nodes, registrationControl = false) {
        for (let index = 0; index < nodes.length; index += fields.length) {
          const infoIndex =
            nodes[index + fields.indexOf('function_info_index')] * infoFields.length;
          const script = snapshot.strings[infos[infoIndex + infoFields.indexOf('script_name')]];
          const name = snapshot.strings[infos[infoIndex + infoFields.indexOf('name')]];
          const objects = nodes[index + fields.indexOf('count')];
          const bytes = nodes[index + fields.indexOf('size')];
          const owner = OWNER_MODULES.find((module) =>
            script.replaceAll('\\', '/').endsWith(module)
          );
          const inRegistrationControl =
            registrationControl ||
            (script === __filename && name === 'requiredRegistrationControl');
          if (owner && objects)
            sites.push({
              module: owner,
              name,
              line: infos[infoIndex + infoFields.indexOf('line')],
              column: infos[infoIndex + infoFields.indexOf('column')],
              objects,
              bytes,
              registrationControl: inRegistrationControl
            });
          // Periodic deadline traversal runs independently of the request count.
          // Keep its actual allocations visible, outside the remote request budget.
          const periodic =
            owner === '/runtime/foundation/operation-registry.js' &&
            (name === 'expire' || name === 'nextDeadlineMs');
          if (
            inRegistrationControl &&
            (owner === '/runtime/foundation/operation-registry.js' ||
              (script === __filename && name === 'calibrationClockNow'))
          ) {
            // Index reads and empty-index resets belong to the full turn,
            // not the entry/id/clock/deadline registration reference.
            const target = ['notePending', 'resetEmptyIndex'].includes(name)
              ? registrationIndex
              : requiredRegistration;
            target.objects += objects;
            target.bytes += bytes;
          } else if (periodic) {
            maintenance.objects += objects;
            maintenance.bytes += bytes;
          } else if (owner) {
            modules[owner].objects += objects;
            modules[owner].bytes += bytes;
          }
          if (script === __filename && name === 'allocationControl') {
            control.objects += objects;
            control.bytes += bytes;
          }
          if (script === __filename && name === 'identityControl') {
            identity.objects += objects;
            identity.bytes += bytes;
          }
          visit(nodes[index + fields.indexOf('children')], inRegistrationControl);
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
      return {
        node: process.version,
        count,
        control,
        maintenance,
        modules,
        total,
        requiredRegistration,
        registrationIndex,
        identity,
        sites
      };
    };
    // Fixed windows measure steady per-call cost rather than retrying an
    // assertion. V8 can attribute one-time VM work to a registration's stack.
    // Each operation has its own steady cost: VM work can affect the reference
    // and the request in different windows. Minimize each across the same fixed
    // number of calls, rather than selecting by a possibly noisy control count.
    const samples = [];
    for (let window = 0; window < 5; window++) samples.push(await measure());
    const completionPerCall = (sample) =>
      (sample.total.objects -
        sample.modules['/runtime/backend/node/node-raw-binding-port.js'].objects) /
      sample.count;
    const minimumPerCall = (cost) =>
      samples.reduce((best, sample) =>
        cost(sample) / sample.count < cost(best) / best.count ? sample : best
      );
    const registration = minimumPerCall((sample) => sample.requiredRegistration.objects);
    const completion = minimumPerCall(
      (sample) =>
        sample.total.objects -
        sample.modules['/runtime/backend/node/node-raw-binding-port.js'].objects
    );
    const control = minimumPerCall((sample) => sample.control.objects);
    return {
      ...completion,
      control: control.control,
      requiredRegistration: registration.requiredRegistration,
      registrationIndex: registration.registrationIndex,
      sites: [
        ...completion.sites.filter((site) => !site.registrationControl),
        ...registration.sites.filter((site) => site.registrationControl)
      ],
      windows: samples.map((sample) => ({
        control: sample.control.objects,
        requiredRegistration: sample.requiredRegistration.objects,
        completionPerCall: completionPerCall(sample)
      })),
      selectedWindows: {
        control: samples.indexOf(control),
        registration: samples.indexOf(registration),
        completion: samples.indexOf(completion)
      }
    };
  } finally {
    session.disconnect();
    registrationRegistry.close();
    await pair.close();
  }
}
module.exports = { trace };
if (require.main === module)
  trace({ registryOnly: process.env.ZLINK_TRACE_REGISTRY_ONLY === '1' }).then(
    (result) => process.stdout.write(`${JSON.stringify(result)}\n`),
    (error) => {
      console.error(error);
      process.exitCode = 1;
    }
  );
