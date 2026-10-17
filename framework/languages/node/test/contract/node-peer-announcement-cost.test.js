'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');

function admittedFixture() {
  const runtime = Object.create(RawServiceMeshRuntime.prototype);
  Object.assign(runtime, { closed: false, selectedRoutes: new Map([['peer', 1n]]), controlSends: new Map(),
    topology: { routeAdmission: () => ({ connectionId: 'route:1', remoteAdmitReceived: true }) } });
  return runtime;
}

test('admitted, no-route and closed announcements return their normal decline synchronously', () => {
  const runtime = admittedFixture();
  assert.strictEqual(runtime.announcePeer('peer'), false);
  assert.strictEqual(runtime.announcePeer('no-route'), false);
  runtime.closed = true;
  assert.strictEqual(runtime.announcePeer('peer'), false);
});

test('a sealed announcement removes an existing HELLO while preserving other control state', () => {
  const runtime = admittedFixture();
  runtime.peerAdmissionSealed = () => true;
  runtime.topology.routeAdmission = () => undefined;
  runtime.topology.localDescriptor = () => { throw new Error('sealed peer encoded a descriptor'); };
  let helloKey;
  const originalDelete = runtime.controlSends.delete;
  runtime.controlSends.delete = function (key) { helloKey = key; return originalDelete.call(this, key); };
  runtime.announcePeer('peer');
  const hello = { hello: Buffer.from('pending') }, other = { task: 'other control' };
  runtime.controlSends.set(helloKey, hello);
  runtime.controlSends.set(helloKey + ':other', other);
  assert.strictEqual(runtime.announcePeer('peer'), false);
  assert.equal(runtime.controlSends.has(helloKey), false);
  assert.strictEqual(runtime.controlSends.get(helloKey + ':other'), other);
});
