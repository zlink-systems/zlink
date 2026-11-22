'use strict';

const assert = require('node:assert/strict');
const { setImmediate: boundary } = require('node:timers/promises');
const test = require('node:test');
const {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkRuntimeState
} = require('../../packages/framework/dist');
const {
  DefaultZLinkSpotOutbound,
  ZLinkHostSpotAddressTransport,
  ZLinkSpotSerialTurnExecutor
} = require('../../packages/framework/dist/internal');
const {
  ZLinkNodeRawMeshBackend
} = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const {
  ServiceStatefulRuntime
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
const {
  decodeStatefulHeader
} = require('../../packages/framework/dist/runtime/foundation/service-stateful-wire-codec');
const {
  ZLinkRequestSourceResolver
} = require('../../packages/framework/dist/runtime/locations/request-source-resolver');

const submittedAt = 1700000000000;
const activationTimeoutMs = 100;
const requestTimeoutMs = 50;

async function fixture(t) {
  let elapsed = 0;
  t.mock.method(Date, 'now', () => submittedAt + elapsed);
  t.mock.method(performance, 'now', () => elapsed);
  t.mock.timers.enable({ apis: ['setTimeout'] });
  // Node's built-in AbortSignal.timeout uses timers outside MockTimers.
  t.mock.method(AbortSignal, 'timeout', (ms) => {
    const controller = new AbortController();
    setTimeout(() => controller.abort(new DOMException('Deadline elapsed', 'TimeoutError')), ms);
    return controller.signal;
  });

  let entered;
  const refreshStarted = new Promise((resolve) => {
    entered = resolve;
  });
  let release;
  const refresh = new Promise((resolve) => {
    release = resolve;
  });
  const target = {
    meshName: 'play',
    rid: 'node-b',
    lifecycleGeneration: 7n,
    descriptorRevision: 11n,
    state: ZLinkFrameworkRuntimeState.Serving,
    objectRole: 'server',
    placementWeight: 100,
    ownerId: 'owner',
    leaseGeneration: 1n,
    objectCapabilities: [{ objectKind: 'instance_spot', stableType: 'room' }],
    populationCapacity: { spots: { active: 0, reserved: 0, limit: 1 }, spotTypes: [] }
  };
  let reads = 0;
  const requestSources = new ZLinkRequestSourceResolver({
    async listLiveMeshNodes(_meshName, signal) {
      reads++;
      entered({ signal });
      await refresh;
      return [target];
    },
    async remainingOwnerTokenLeaseMs() {
      return 30000;
    }
  });
  await requestSources.observeLiveDescriptors('play', []);

  const envelopes = [];
  const stateful = new ServiceStatefulRuntime(
    {
      setServiceIngress() {},
      async sendApplication(rid, [header]) {
        assert.equal(rid, target.rid);
        envelopes.push(decodeStatefulHeader(header));
        return true;
      }
    },
    'node-a',
    1n
  );
  const node = new ZLinkNodeRawMeshBackend(
    'play',
    'node-a',
    {},
    undefined,
    undefined,
    undefined,
    undefined,
    { stateful }
  );
  t.mock.method(node, 'isPeerRouteReady', () => true);
  const requestSubmission = t.mock.method(node, 'requestToMissingInstanceSpot', () => {
    assert.fail('An expired request reached transport submission.');
  });
  const shutdown = new AbortController();
  const outbound = new DefaultZLinkSpotOutbound({
    serial: new ZLinkSpotSerialTurnExecutor(),
    addressTransport: new ZLinkHostSpotAddressTransport({
      resolver: () => ({ resolve: async () => undefined }),
      requestSources: () => requestSources,
      shutdownSignal: () => shutdown.signal,
      routed: {},
      meshNames: () => ['play'],
      meshNode: () => node,
      completions: () => undefined,
      defaultRequestTimeoutMs: activationTimeoutMs
    })
  });
  class Packet {}
  return {
    send: () => outbound.sendToSpot('instance', new Packet()).instanceSpot('room').submit(),
    request: () =>
      outbound
        .requestToSpot('instance', new Packet())
        .instanceSpot('room')
        .timeout(requestTimeoutMs)
        .submit(),
    refreshStarted,
    release,
    reads: () => reads,
    advance(ms) {
      elapsed += ms;
      t.mock.timers.tick(ms);
    },
    shutdown,
    envelopes,
    requestSubmission
  };
}

test('Instance Spot send submits with its original activation deadline after a late Store refresh', async (t) => {
  const f = await fixture(t);
  const sent = assert.doesNotReject(f.send());
  await f.refreshStarted;
  f.advance(activationTimeoutMs + 1);
  f.release();
  await sent;
  assert.equal(f.reads(), 1);
  assert.equal(f.envelopes.length, 1);
  assert.equal(f.envelopes[0].activation, 'missing');
  assert.equal(f.envelopes[0].deadlineUnixMs, BigInt(submittedAt + activationTimeoutMs));
});

test('Instance Spot request expires during Store refresh at its end-to-end deadline', async (t) => {
  const f = await fixture(t);
  const expired = assert.rejects(f.request(), { kind: ZLinkFrameworkErrorKind.DeadlineExceeded });
  const { signal } = await f.refreshStarted;
  f.advance(requestTimeoutMs);
  try {
    await expired;
    assert.equal(signal.aborted, true);
  } finally {
    f.release();
  }
  await boundary();
  assert.equal(f.reads(), 1);
  assert.equal(f.requestSubmission.mock.callCount(), 0);
});

test('Instance Spot send ends with ShuttingDown when its Store refresh is interrupted by shutdown', async (t) => {
  const f = await fixture(t);
  const stopped = assert.rejects(f.send(), { kind: ZLinkFrameworkErrorKind.ShuttingDown });
  await f.refreshStarted;
  f.shutdown.abort();
  f.release();
  await stopped;
  assert.equal(f.envelopes.length, 0);
});
