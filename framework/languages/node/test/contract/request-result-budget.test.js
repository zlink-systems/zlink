const assert = require('node:assert/strict');
const test = require('node:test');
const { createHook } = require('node:async_hooks');
const {
  createZLinkRequest: createMappedRequest,
  prepareZLinkRequestReply,
  identityRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-result');

// The source owns its result mapping; the public helper owns admission state.
function createZLinkRequest(start, deliverReply) {
  return createMappedRequest((admitted) =>
    prepareZLinkRequestReply(start(admitted), identityRequestReply), deliverReply);
}

function deferred() {
  let resolve;
  let reject;
  const promise = new Promise((done, fail) => { resolve = done; reject = fail; });
  return { promise, resolve, reject };
}

test('reply-only requests allocate only the public reply observation Promise', async () => {
  const pending = deferred();
  let markAdmitted;
  let count = 0;
  const hook = createHook({ init(_id, type) { if (type === 'PROMISE') count++; } });
  hook.enable();
  let request;
  try {
    request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  } finally {
    hook.disable();
  }
  markAdmitted();
  pending.resolve('reply');
  assert.equal(await request, 'reply');
  await request.admitted;
  assert.equal(count, 1);
});

test('observed admission completes before reply and retains one public Promise', async () => {
  const pending = deferred();
  let markAdmitted;
  const request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  const admitted = request.admitted;
  assert.equal(request.admitted, admitted);
  let replied = false;
  void request.then(() => { replied = true; });
  markAdmitted();
  markAdmitted();
  await admitted;
  assert.equal(replied, false);
  pending.resolve('reply');
  assert.equal(await request, 'reply');
  assert.equal(request.admitted, admitted);
});

test('admission retains its first outcome when a later reply fails', async () => {
  const pending = deferred();
  let markAdmitted;
  const request = createZLinkRequest((admitted) => { markAdmitted = admitted; return pending.promise; });
  const failure = new Error('reply failed after admission');
  const observed = assert.rejects(request, (error) => error === failure);
  markAdmitted();
  pending.reject(failure);
  await observed;
  await request.admitted;
});

test('a failure before admission rejects both lazy public stages with the same cause', async () => {
  const failure = new Error('submit failed');
  const request = createZLinkRequest(() => { throw failure; });
  const observed = assert.rejects(request, (error) => error === failure);
  await observed;
  await assert.rejects(request.admitted, (error) => error === failure);
  assert.equal(request.admitted, request.admitted);
});

test('one request admission failure does not alter a completed or waiting sibling stage', async () => {
  const left = deferred();
  const right = deferred();
  let admitLeft;
  const first = createZLinkRequest((admitted) => { admitLeft = admitted; return left.promise; });
  const second = createZLinkRequest(() => right.promise);
  const failure = new Error('sibling admission failed');
  const observed = assert.rejects(second, (error) => error === failure);
  const secondAdmission = second.admitted;
  const admissionFailure = assert.rejects(secondAdmission, (error) => error === failure);
  admitLeft();
  await first.admitted;
  right.reject(failure);
  await Promise.all([observed, admissionFailure]);
  await first.admitted;
  left.resolve('first reply');
  assert.equal(await first, 'first reply');
});

test('admission notification and reply mapping use the same admission owner', async () => {
  const pending = deferred();
  const failure = new Error('late decode failure');
  let markAdmitted;
  const request = createMappedRequest((admitted) => {
    markAdmitted = admitted;
    return prepareZLinkRequestReply(pending.promise, () => { throw failure; });
  });
  const admitted = request.admitted;
  markAdmitted();
  await admitted;
  pending.resolve('reply');
  await assert.rejects(request, (error) => error === failure);
  assert.equal(request.admitted, admitted);
  await admitted;
});

test('reply delivery retains its Promise identity and admission getter attributes', async () => {
  const pending = deferred();
  let delivered;
  let deliveries = 0;
  const request = createZLinkRequest(
    () => pending.promise,
    (reply) => {
      deliveries++;
      delivered = reply.then((value) => value);
      return delivered;
    }
  );
  const descriptor = Object.getOwnPropertyDescriptor(request, 'admitted');
  assert.equal(request, delivered);
  assert.equal(request.constructor, Promise);
  assert.equal(Object.getPrototypeOf(request), Promise.prototype);
  assert.equal(deliveries, 1);
  assert.equal(descriptor.enumerable, false);
  assert.equal(descriptor.configurable, false);
  assert.equal(descriptor.set, undefined);
  assert.equal(Object.hasOwn(pending.promise, 'admitted'), false);
  const admitted = request.admitted;
  pending.resolve(undefined);
  assert.equal(await request, undefined);
  await admitted;
  assert.equal(request.admitted, admitted);
});

// A getter closure per request gives every public reply Promise its own hidden
// class; V8 then stores each request's properties in a dictionary. The getter
// is shared so request results keep one shape.
test('public requests share one admission getter', async () => {
  const requests = [0, 1].map(() => createZLinkRequest((admitted) => {
    admitted();
    return Promise.resolve('reply');
  }));
  const [first, second] = requests.map((request) => Object.getOwnPropertyDescriptor(request, 'admitted'));
  assert.equal(typeof first.get, 'function');
  assert.equal(first.get, second.get);
  assert.deepEqual(Object.keys(requests[0]), []);
  for (const request of requests) {
    assert.equal(await request, 'reply');
    await request.admitted;
    assert.equal(request.admitted, request.admitted);
  }
});

test('reply-only failure is observed before a later admission read', async () => {
  const failure = new DOMException('cancelled before admission', 'AbortError');
  const request = createZLinkRequest(() => Promise.reject(failure));
  await assert.rejects(request, (error) => error === failure);
  // Give unhandled rejection reporting a turn before observing admission.
  await new Promise((resolve) => setImmediate(resolve));
  const admitted = request.admitted;
  await assert.rejects(admitted, (error) => error === failure);
  assert.equal(request.admitted, admitted);
});

test('a synchronous failure after admission preserves the completed admission', async () => {
  const failure = new Error('failed after admission');
  const request = createZLinkRequest((admitted) => {
    admitted();
    throw failure;
  });
  await assert.rejects(request, (error) => error === failure);
  const admitted = request.admitted;
  await admitted;
  assert.equal(request.admitted, admitted);
});

for (const failed of [false, true]) {
  test(`prepared reply retains its captured flow in a later completion turn (failure=${failed})`, async () => {
    const { currentFlowContext, runWithFlow } = require('../../packages/framework/dist/runtime/diagnostics/flow-context');
    const flow = { flowId: 'captured', flowOrigin: 'Application' };
    const pending = deferred();
    const cause = new Error('reply failure');
    let observed = false;
    const result = runWithFlow(flow, () => prepareZLinkRequestReply(
      pending.promise,
      value => { observed = true; assert.equal(currentFlowContext(), flow); return value; },
      error => { observed = true; assert.equal(currentFlowContext(), flow); return error; }
    ));
    assert.equal(currentFlowContext(), undefined);
    const reply = createMappedRequest(() => result);
    const settled = failed ? assert.rejects(reply, error => error === cause) : reply;
    if (failed) pending.reject(cause); else pending.resolve(7);
    assert.equal(observed, false, 'mapping must not run in the terminal publication stack');
    await settled;
    assert.equal(observed, true);
    assert.equal(currentFlowContext(), undefined);
  });
}
