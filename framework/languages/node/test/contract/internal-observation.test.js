'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist');
const { inspectRuntime } = require('../../packages/framework/dist/runtime/internal-observation');
const { ZLinkFrameworkRuntimeHost } = require('../../packages/framework/dist/runtime/host');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const { ZLinkStoreLocationResolvers } = require('../../packages/framework/dist/runtime/locations/resolvers');
const { ServiceDiscoveryRegistry } = require('../../packages/framework/dist/runtime/foundation/service-discovery-registry');
const { ZLinkChannelSocketRegistry } = require('../../packages/framework/dist/runtime/channels/channel-socket-registry');
const { createFrameworkRegistration } = require('../../packages/framework/dist/runtime/configuration');
const { rawMeshFixture } = require('./fixtures/raw-mesh-binding');

function assertReadOnlyValue(value) {
  if (value === undefined || value === null || typeof value !== 'object') return;
  assert.ok(Object.isFrozen(value));
  for (const [field, item] of Object.entries(value)) {
    assertReadOnlyValue(item);
    assert.throws(() => { value[field] = 'changed'; }, TypeError);
  }
  if (Array.isArray(value)) assert.throws(() => { value.push('changed'); }, TypeError);
}

function assertValueSnapshot(owner) {
  const snapshot = owner[inspectRuntime]();
  assertReadOnlyValue(snapshot);
  const next = owner[inspectRuntime]();
  assert.notStrictEqual(next, snapshot);
  for (const [field, value] of Object.entries(snapshot)) {
    if (typeof value === 'function') {
      // Query views return copied values, never a mutable registry entry.
      assert.equal(value('missing'), undefined);
      assert.equal(next[field]('missing'), undefined);
    } else {
      assert.deepEqual(next[field], value);
    }
  }
}

test('internal observers return frozen value copies and are absent from the public entry', () => {
  assert.equal(Object.hasOwn(framework, 'inspectRuntime'), false);
  const registration = createFrameworkRegistration();
  const raw = rawMeshFixture();
  try {
    const owners = [
      raw.runtime,
      new ServiceStatefulRuntime(raw.runtime, 'local', 1n),
      new ZLinkHostServiceRelocationRuntime({}),
      new ZLinkFrameworkRuntimeHost({ registration }),
      new ServiceDiscoveryRegistry(),
      new ZLinkStoreLocationResolvers({ stores: {}, leaseTracker: {} }),
      new ZLinkChannelSocketRegistry(registration, {}, {})
    ];
    owners.forEach(assertValueSnapshot);
  } finally {
    raw.runtime.close();
  }
});

test('observer copies retain their value after the owned subscription ends', () => {
  const registry = new ServiceDiscoveryRegistry();
  const unsubscribe = registry.onClientServerChanged(() => {});
  const before = registry[inspectRuntime]();
  assert.equal(before.clientServerObservers, 1);
  unsubscribe();
  assert.equal(registry[inspectRuntime]().clientServerObservers, 0);
  assert.equal(before.clientServerObservers, 1);
});
