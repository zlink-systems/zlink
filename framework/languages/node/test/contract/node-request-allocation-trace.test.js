'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const path = require('node:path');
const { spawnSync } = require('node:child_process');

function trace(registryOnly = false) {
  const result = spawnSync(
    process.execPath,
    ['--no-opt', '--expose-gc', path.join(__dirname, 'fixtures/typed-request-allocation-trace.js')],
    {
      encoding: 'utf8',
      env: {
        ...process.env,
        NODE_TEST_CONTEXT: undefined,
        ZLINK_TRACE_REGISTRY_ONLY: registryOnly ? '1' : '0'
      }
    }
  );
  assert.ifError(result.error);
  assert.equal(result.status, 0, `${result.stdout}\n${result.stderr}`);
  return JSON.parse(result.stdout.trim());
}

// Spec 08 E3 requires the registry entry and its deadline. With --no-opt,
// The fractional clock reference materializes the bigint id, entry, clock value
// and deadline value as four containers. The id was already in the old budget.
// Calibrate the added three in the same snapshot, and separately reject any
// extra per-request allocation in the registration implementation itself.
// Both cases use the same GC and compilation flags, and a fixed set of paired
// windows separates steady per-call cost from one-time VM allocations.
function registrationOverhead(metrics) {
  assert.ok(metrics.identity.objects >= metrics.count, 'identity control is observed');
  assert.ok(
    metrics.requiredRegistration.objects >= metrics.count,
    'required registration is observed'
  );
  assert.ok(
    metrics.requiredRegistration.objects <= 4 * metrics.control.objects,
    'registration adds containers beyond the required entry, identity and deadline values'
  );
  // --no-opt boxes one earliest-deadline read and two Infinity resets.
  // Keep this fixed index cost visible and bounded separately from registration.
  assert.ok(metrics.registrationIndex.objects >= metrics.count, 'deadline index is observed');
  assert.ok(
    metrics.registrationIndex.objects <= 3 * metrics.control.objects,
    'deadline index adds containers beyond one read and two empty-index resets'
  );
  const registered = metrics.sites
    .filter(
      (site) =>
        !site.registrationControl &&
        site.module === '/runtime/foundation/operation-registry.js' &&
        ['registerObserved', 'notePending'].includes(site.name)
    )
    .reduce((sum, site) => sum + site.objects, 0);
  assert.ok(
    registered <= metrics.requiredRegistration.objects,
    'request registration exceeds the standalone required registration control'
  );
  return Math.max(0, metrics.requiredRegistration.objects - metrics.identity.objects);
}

test('registry allocation trace includes required registration and rejects an allocating control', (t) => {
  const metrics = trace(true);
  t.diagnostic(JSON.stringify(metrics));
  assert.ok(registrationOverhead(metrics) > 0);
  const allocating = structuredClone(metrics);
  allocating.requiredRegistration.objects += metrics.control.objects;
  assert.throws(() => registrationOverhead(allocating), /beyond the required entry/);
  const indexAllocating = structuredClone(metrics);
  indexAllocating.registrationIndex.objects += metrics.control.objects;
  assert.throws(() => registrationOverhead(indexAllocating), /deadline index adds containers/);
  const requestAllocating = structuredClone(metrics);
  // Exceed the whole reference even when clock boxes are attributed outside
  // registerObserved. The positive registration bound remains unchanged.
  requestAllocating.sites.push({
    module: '/runtime/foundation/operation-registry.js',
    name: 'registerObserved',
    objects: metrics.requiredRegistration.objects,
    registrationControl: false
  });
  assert.throws(() => registrationOverhead(requestAllocating), /exceeds the standalone/);
});

test('V8 trace records completion owner allocations and an allocating control', (t) => {
  const metrics = trace();
  t.diagnostic(JSON.stringify(metrics));
  assert.equal(metrics.count, 100);
  assert.ok(metrics.control.objects >= metrics.count, 'allocating control is observed');
  assert.ok(metrics.total.objects > metrics.count, 'request allocations are observed');
  assert.ok(metrics.total.bytes > 0, 'request bytes are observed');
  // Compare object counts with closures allocated in this same V8 run. The
  // completion-owner budget remains 23 containers per request, plus the
  // independently checked required deadline registration (spec 08 E3).
  // Periodic traversal and native observation remain separately visible.
  const observer = metrics.modules['/runtime/backend/node/node-raw-binding-port.js'];
  const completionObjects = metrics.total.objects - observer.objects;
  assert.ok(observer.objects >= metrics.count, 'binding observer container is observed');
  assert.ok(completionObjects >= metrics.count, 'completion owner is observed');
  const budget = 23 * metrics.control.objects + registrationOverhead(metrics);
  assert.ok(
    completionObjects <= budget,
    `${completionObjects} completion objects exceed ${budget} control containers including required registration`
  );
});
