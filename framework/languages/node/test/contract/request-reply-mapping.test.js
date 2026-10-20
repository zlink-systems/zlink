const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const framework = require('../../packages/framework/dist/internal');
const {
  createZLinkRequest, prepareZLinkRequestReply, identityRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-result');

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

function fixture() {
  const source = deferred();
  const node = { requestToNode: () => ({ high: 1n, low: 1n }) };
  const table = { submit(operation) { operation(); return source.promise; } };
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined, undefined,
    () => ({ meshNode: () => node, meshCompletionTable: () => table }),
    undefined, undefined, undefined, undefined, () => false
  );
  transport.encodeMessage = () => [];
  transport.decodeMeshReply = (_mesh, value) => value;
  const registration = framework.createFrameworkRegistration({
    spotNodes: { mesh: { router: { bind: 'tcp://127.0.0.1:0' } } }
  });
  const client = new framework.DefaultZLinkRouteClient(registration, transport);
  return { source, transport, node, client };
}

// Spec 08 E5 and Submit §11 require one new caller turn. Typed decode and
// admission confirmation consume the same result within that turn.
test('typed completion and public admission execute one result reaction', async () => {
  const f = fixture();
  const reactions = new Set();
  let capture = false, callbacks = 0;
  const hook = createHook({
    init(id, type) { if (capture && type === 'PROMISE') reactions.add(id); },
    before(id) { if (reactions.has(id)) callbacks++; }
  });
  hook.enable();
  let request;
  try {
    capture = true;
    request = f.client.requestToNode('mesh', 'peer', {}).submit();
    capture = false;
    f.source.resolve('reply');
    assert.equal(await request, 'reply');
    await request.admitted;
    assert.equal(callbacks, 1, 'one typed-result/admission reaction, with no wrapper forwarding');
  } finally { capture = false; hook.disable(); }
});

for (const cause of ['decode', 'submission', 'encode']) {
  test(`${cause} failure reaches both public stages with the same cause`, async () => {
    const f = fixture();
    const error = new Error(cause);
    if (cause === 'decode') f.transport.decodeMeshReply = () => { throw error; };
    if (cause === 'encode') f.transport.encodeMessage = () => { throw error; };
    const request = f.client.requestToNode('mesh', 'peer', {}).submit();
    const observed = Promise.allSettled([request, request.admitted]);
    if (cause === 'submission') f.source.reject(error);
    else f.source.resolve('reply');
    const outcomes = await observed;
    assert.equal(outcomes[0].status, 'rejected');
    assert.equal(outcomes[1].status, 'rejected');
    assert.equal(outcomes[0].reason, outcomes[1].reason);
    if (cause === 'submission') assert.equal(outcomes[0].reason.cause, error);
    else assert.equal(outcomes[0].reason, error);
  });
}

for (const throws of [false, true]) {
  test(`failure mapping preserves admission when mapper throws=${throws}`, async () => {
    const source = deferred();
    const error = new Error('mapped failure');
    const request = createZLinkRequest((_admitted) => prepareZLinkRequestReply(source.promise, identityRequestReply, () => { if (throws) throw error; return error; }));
    const observed = Promise.allSettled([request, request.admitted]);
    source.reject(new Error('source failure'));
    const outcomes = await observed;
    assert.equal(outcomes[0].reason, outcomes[1].reason);
    assert.equal(outcomes[0].reason, error);
  });
}

test('typed decode and caller run after terminal publication returns', async () => {
  const f = fixture();
  let publishing = true;
  f.transport.decodeMeshReply = (_mesh, value) => {
    assert.equal(publishing, false);
    return value;
  };
  const request = f.client.requestToNode('mesh', 'peer', {}).submit();
  f.source.resolve('reply');
  publishing = false;
  assert.equal(await request, 'reply');
});

for (const kind of ['client-server', 'source-spot']) {
  test(`${kind} synchronous forwarding failure remains a rejected Promise`, async () => {
    const error = new Error('forward failed');
    let transport;
    if (kind === 'client-server') {
      const { ZLinkChannelRuntimeManager } = require('../../packages/framework/dist/runtime/channels/channel-runtime-manager');
      transport = Object.create(ZLinkChannelRuntimeManager.prototype);
      transport.outbound = { request() { throw error; } };
    } else {
      transport = new framework.ZLinkRuntimeRouteTransport(() => { throw error; });
    }
    let pending;
    const request = createZLinkRequest((admitted) => {
      assert.doesNotThrow(() => {
        pending = kind === 'client-server'
          ? transport.request('channel', undefined, {}, 1000, undefined, undefined, admitted)
          : transport.requestFromSpotToSpot({}, {}, {}, { onAdmitted: admitted });
      });
      return pending;
    });
    assert.ok(pending.pending instanceof Promise);
    assert.ok(request instanceof Promise);
    const outcomes = await Promise.allSettled([request, request.admitted]);
    assert.equal(outcomes[0].reason, error);
    assert.equal(outcomes[1].reason, error);
  });
}

test('channel synchronous completion submission failure records the original metric outcome', async () => {
  const { createInternalFrameworkException, ZLinkFrameworkInternalErrorKind } = require('../../packages/framework/dist/runtime/framework-errors-internal');
  const error = createInternalFrameworkException(ZLinkFrameworkInternalErrorKind.RequestTargetNotFound, 'missing');
  const metrics = [];
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined, undefined,
    () => ({ meshNode: () => ({}), meshCompletionTable: () => { throw error; } }),
    undefined, undefined, undefined,
    {
      startRequest: () => ({ complete: (outcome) => metrics.push(outcome) }),
      recordChannelSelectionFailure: (...args) => metrics.push(args)
    },
    () => false
  );
  transport.encodeMessage = () => [];
  const request = createZLinkRequest((admitted) =>
    transport.requestToChannel('mesh', 'channel', undefined, {}, 1000, undefined, undefined, admitted));
  const outcomes = await Promise.allSettled([request, request.admitted]);
  assert.equal(outcomes[0].reason, error);
  assert.equal(outcomes[1].reason, error);
  assert.equal(metrics.length, 2);
  assert.deepEqual(metrics[1], ['mesh', 'channel', 'no_ready_target']);
});

for (const failureAt of ['handle conversion', 'transport call']) {
  test(`SpotHandle ${failureAt} synchronous failure rejects admission`, async () => {
    const error = new Error(failureAt);
    const registration = framework.createFrameworkRegistration({
      spotNodes: { mesh: { router: { bind: 'inproc://reply-mapping' } } }
    });
    const client = new framework.DefaultZLinkRouteClient(registration, {
      sendToSpot() {},
      requestToSpot() { throw error; }
    }, failureAt === 'handle conversion' ? () => { throw error; } : undefined);
    const target = { meshName: 'mesh', nodeRid: 'peer', spotId: 'target', spotKind: framework.ZLinkSpotKind.Entry };
    const handle = framework.createSpotHandle('target', target, async () => target);
    class Query {}
    const request = client.requestToSpot(handle, new Query()).submit();
    const failure = await request.catch((cause) => cause);
    assert.equal(failure, error);
    let admissionFailure;
    void request.admitted.catch((cause) => { admissionFailure = cause; });
    // A settled admission callback precedes this marker; a missing rejection stays pending.
    await Promise.resolve();
    assert.equal(admissionFailure, failure);
  });
}
