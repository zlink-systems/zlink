'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { RawServiceMeshRuntime } = require('../../packages/framework/dist/runtime/foundation/raw-service-mesh-runtime');
const { M6aServiceWireCommand } = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

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

// MeshNode §7.1 retains HELLO until ADMIT; host admission can resume after
// Location runtime §5 renews the owner lease. Pausing a send is not a terminal.
test('a sealed announcement preserves HELLO and other controls without encoding', () => {
  const runtime = admittedFixture();
  runtime.peerAdmissionSealed = () => true;
  runtime.topology.routeAdmission = () => undefined;
  runtime.topology.localDescriptor = () => { throw new Error('sealed peer encoded a descriptor'); };
  const helloKey = `${M6aServiceWireCommand.hello}:peer`;
  const otherKey = `${M6aServiceWireCommand.livenessProbe}:other`;
  const hello = { hello: Buffer.from('pending'), task: Promise.resolve(false) };
  const other = { task: Promise.resolve(false) };
  runtime.controlSends.set(helloKey, hello);
  runtime.controlSends.set(otherKey, other);
  assert.strictEqual(runtime.announcePeer('peer'), false);
  assert.strictEqual(runtime.controlSends.get(helloKey), hello);
  assert.strictEqual(runtime.controlSends.get(otherKey), other);
});

test('a route first observed while sealed retains only its HELLO intent', () => {
  const runtime = admittedFixture();
  runtime.peerAdmissionSealed = () => true;
  runtime.topology.routeAdmission = () => undefined;
  runtime.topology.localDescriptor = () => { throw new Error('sealed peer encoded a descriptor'); };
  assert.strictEqual(runtime.announcePeer('peer'), false);
  const intent = runtime.controlSends.get(`${M6aServiceWireCommand.hello}:peer`);
  assert.ok(intent);
  assert.equal(intent.hello, undefined);
  assert.equal(intent.task, undefined);
  assert.equal(intent.current(), false);
  runtime.peerAdmissionSealed = () => false;
  assert.equal(intent.current(), true);
});
