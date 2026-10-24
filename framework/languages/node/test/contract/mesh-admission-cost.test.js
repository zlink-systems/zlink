const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const backend = require('../../packages/framework/dist/runtime/backend');
const { ZLinkRuntimeAdmissionGate } = require('../../packages/framework/dist/runtime/admission');
const {
  ZLinkRoutePacketDispatcher
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const {
  encodeChannelEnvelopeParts
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');

function assertIdentities(observed, shared, baseline, count) {
  assert.equal(new Set([...observed].filter((fn) => !shared.has(fn))).size, baseline + count);
}

test('mesh host admission retains only its one per-record callback', async (t) => {
  const mesh = `admission-cost-${process.pid}`;
  const functions = new Set();
  let admissions = 0;
  let calls = 0;
  let replies = 0;
  class Ping {
    handle() {
      calls++;
      return { ok: true };
    }
  }
  const registration = framework.createFrameworkRegistrationWithBuilder((builder) => {
    builder
      .addRouteMesh(mesh)
      .listen(`inproc://${mesh}`)
      .routingId('cost-node')
      .addRouteRequestHandler(Ping);
  });
  const factory = new backend.ZLinkNodeBackendAdapterFactory();
  const adapter = factory.createMeshAdapter();
  const create = adapter.createMeshNode.bind(adapter);
  let ready;
  let batch;
  let finished;
  t.mock.method(factory, 'createMeshAdapter', () => adapter);
  t.mock.method(adapter, 'createMeshNode', (...args) => {
    const node = create(...args);
    t.mock.method(node, 'setReadyHandler', (handler) => {
      ready = handler;
    });
    t.mock.method(node, 'createReadyBatch', () => ({
      reset() {},
      close() {},
      takeClaim() {
        let received = false;
        return {
          recvBatch() {
            if (received) return { ok: false, records: [] };
            received = true;
            return { ok: true, records: [batch.shift()] };
          },
          release() {
            if (batch.length === 0) finished();
          }
        };
      }
    }));
    t.mock.method(node, 'createReceiveBatch', () => ({ reset() {}, close() {} }));
    t.mock.method(node, 'drainReady', () => {
      if (!batch?.length) return { ok: false, records: [], hasResidue: false };
      return {
        ok: true,
        records: [
          { ownerKind: framework.ReadyOwnerKind.Node, domain: framework.ReadyDomain.Application }
        ],
        hasResidue: false
      };
    });
    return node;
  });
  const dispatchMesh = ZLinkRoutePacketDispatcher.prototype.dispatchMesh;
  t.mock.method(ZLinkRoutePacketDispatcher.prototype, 'dispatchMesh', function (...args) {
    functions.add(args[2]);
    return dispatchMesh.apply(this, args);
  });
  const run = ZLinkRuntimeAdmissionGate.prototype.run;
  t.mock.method(ZLinkRuntimeAdmissionGate.prototype, 'run', function (...args) {
    admissions++;
    for (const value of args) if (typeof value === 'function') functions.add(value);
    return run.apply(this, args);
  });
  const host = new framework.ZLinkFrameworkRuntimeHost(
    { registration },
    { backendAdapterFactory: factory }
  );
  async function ingress(count) {
    batch = Array.from({ length: count }, () => ({
      kind: framework.ReceiveKind.NodeRequest,
      domain: framework.ReadyDomain.Application,
      sourceNodeRid: 'source',
      parts: encodeChannelEnvelopeParts(1, mesh, 'Ping', { bytes: 'x'.repeat(1024) }).map(
        (data) => ({ data: () => data, close() {} })
      ),
      reply() {
        replies++;
        return backend.SubmitResult.Ok;
      }
    }));
    const done = new Promise((resolve) => {
      finished = resolve;
    });
    ready(framework.ReadyDomain.Application);
    await done;
  }
  try {
    await host.start();
    await ingress(8);
    const shared = new Set(functions);
    functions.clear();
    admissions = calls = replies = 0;
    const count = 12;
    await ingress(count);
    assert.equal(admissions, count);
    assert.equal(calls, count);
    assert.equal(replies, count);
    const observed = new Set(functions);
    // Direct admission with already prepared functions creates no new function identities.
    const gate = new ZLinkRuntimeAdmissionGate();
    gate.register(mesh);
    function work() {}
    function rejected() {}
    functions.clear();
    gate.run(mesh, 'direct warmup', work, undefined, rejected);
    const baselineShared = new Set(functions);
    functions.clear();
    for (let i = 0; i < count; i++) {
      gate.run(mesh, 'direct baseline', work, undefined, rejected);
    }
    const baselineNew = [...functions].filter((fn) => !baselineShared.has(fn)).length;
    assert.equal(baselineNew, 0);
    t.diagnostic(
      JSON.stringify({
        records: count,
        baselineNew,
        newFunctionIdentities: [...observed].filter((fn) => !shared.has(fn)).length
      })
    );
    assertIdentities(observed, shared, baselineNew, count);
  } finally {
    await host.stop();
  }
});

function dispatchFixture(request, handler) {
  const gate = new ZLinkRuntimeAdmissionGate();
  gate.register('negative');
  const dispatcher = new ZLinkRoutePacketDispatcher({
    routerChannelId: 'negative',
    handlers: [{ kind: request ? 'request' : 'send', packetName: 'Ping', handler }],
    dispatchErrors: { flow: { flowCreationEnabled: () => false, begin() {} }, report() {} }
  });
  let replies = 0;
  const record = {
    kind: request ? framework.ReceiveKind.NodeRequest : framework.ReceiveKind.NodeSend,
    sourceNodeRid: 'source',
    parts: encodeChannelEnvelopeParts(request ? 1 : 3, 'negative', 'Ping', {}).map((data) => ({
      data: () => data,
      close() {}
    })),
    reply() {
      replies++;
      return backend.SubmitResult.Ok;
    }
  };
  return { gate, dispatcher, record, replies: () => replies };
}

test('function identity assertion detects bind wrappers actually submitted to admission', (t) => {
  let calls = 0;
  const fixture = dispatchFixture(false, {
    handle() {
      calls++;
    }
  });
  const observed = new Set();
  const run = fixture.gate.run;
  t.mock.method(fixture.gate, 'run', function (...args) {
    for (const value of args) if (typeof value === 'function') observed.add(value);
    return run.apply(this, args);
  });
  for (let i = 0; i < 12; i++) {
    const admit = (work, rejected, receiver, context) =>
      fixture.gate.run(
        'negative',
        'bound ingress',
        work.bind(receiver),
        fixture.record.kind,
        rejected.bind(receiver),
        undefined,
        context
      );
    observed.add(admit);
    fixture.dispatcher.dispatchMesh(fixture.record, undefined, admit);
  }
  assert.equal(calls, 12);
  assert.throws(() => assertIdentities(observed, new Set(), 0, 12), assert.AssertionError);
});

test('reply assertion detects an admission wrapper dropping the domain failure callback', async () => {
  const fixture = dispatchFixture(true, {
    handle() {
      assert.fail('sealed request reached handler');
    }
  });
  fixture.gate.seal('negative');
  await assert.rejects(async () =>
    fixture.dispatcher.dispatchMesh(fixture.record, undefined, (work) =>
      fixture.gate.run('negative', 'dropped rejection', work)
    )
  );
  assert.throws(() => assert.equal(fixture.replies(), 1), assert.AssertionError);
});
