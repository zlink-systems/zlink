const assert = require('node:assert/strict');
const path = require('node:path');
const test = require('node:test');

// A separate Node checkout permits verification against #2351 before it merges.
const nodeRoot = process.env.ZLINK_SUBMISSION_NODE_ROOT || path.resolve(__dirname, '../../node');
const framework = require(path.join(nodeRoot, 'packages/framework/dist/internal'));
const { createZLinkRequest } = require(
  path.join(nodeRoot, 'packages/framework/dist/runtime/messaging/request-result')
);

function request(pathName, failure) {
  const table = new framework.ZLinkMeshCompletionTable();
  let submissions = 0;
  const submit = () => {
    submissions++;
    throw failure;
  };
  const node = {
    requestToNode: submit,
    requestToChannel: submit,
    entrySpot: () => ({ requestToSpot: submit })
  };
  const transport = new framework.ZLinkRuntimeRouteTransport(
    () => undefined,
    undefined,
    () => ({ meshNode: () => node, meshCompletionTable: () => table }),
    undefined,
    undefined,
    undefined,
    undefined,
    () => false
  );
  const target = {
    routerChannelId: 'mesh',
    targetNodeRid: 'peer',
    spotId: 'entry',
    spotKind: framework.ZLinkSpotKind.Entry
  };
  const pending = createZLinkRequest((onAdmitted) => {
    if (pathName === 'node')
      return transport.request(
        'mesh',
        'peer',
        'Probe',
        { value: 1 },
        1000,
        undefined,
        undefined,
        onAdmitted
      );
    if (pathName === 'channel')
      return transport.requestToChannel(
        'mesh',
        'channel',
        'Probe',
        { value: 1 },
        1000,
        undefined,
        undefined,
        onAdmitted
      );
    return transport.requestToSpot(
      target,
      { value: 1 },
      {
        packetName: 'Probe',
        timeoutMs: 1000,
        onAdmitted
      }
    );
  });
  return { pending, table, submissions: () => submissions };
}

for (const pathName of ['node', 'channel', 'spot']) {
  test(`${pathName} unclassified submission failure preserves its cause`, async () => {
    const original = new Error('unclassified backend submission failure');
    const f = request(pathName, original);
    const outcomes = await Promise.allSettled([f.pending, f.pending.admitted]);
    for (const outcome of outcomes) {
      assert.equal(outcome.status, 'rejected');
      assert.ok(outcome.reason instanceof framework.ZLinkFrameworkException);
      assert.equal(outcome.reason.kind, framework.ZLinkFrameworkErrorKind.InternalFailure);
      assert.equal(outcome.reason.cause, original);
    }
    assert.equal(outcomes[0].reason, outcomes[1].reason);
    assert.equal(f.submissions(), 1);
    assert.equal(f.table.pendingCount, 0);
  });

  test(`${pathName} submission preserves caller and typed failures`, async () => {
    for (const original of [
      new TypeError('invalid argument'),
      new RangeError('invalid range'),
      new framework.ZLinkConfigurationException('invalid configuration'),
      new DOMException('cancelled', 'AbortError'),
      new framework.ZLinkFrameworkException(
        framework.ZLinkFrameworkErrorKind.Rejected,
        'classified failure'
      )
    ]) {
      const f = request(pathName, original);
      for (const outcome of await Promise.allSettled([f.pending, f.pending.admitted])) {
        assert.equal(outcome.status, 'rejected');
        assert.equal(outcome.reason, original);
      }
      assert.equal(f.submissions(), 1);
      assert.equal(f.table.pendingCount, 0);
    }
  });

  test(`${pathName} submission retains typed backend classification`, async () => {
    const original = new framework.ZLinkBackendResultError(
      'submit',
      framework.SubmitResult.Terminated,
      0,
      { phase: 'submit' }
    );
    const f = request(pathName, original);
    for (const outcome of await Promise.allSettled([f.pending, f.pending.admitted])) {
      assert.equal(outcome.status, 'rejected');
      assert.equal(outcome.reason.kind, framework.ZLinkFrameworkErrorKind.ShuttingDown);
      assert.equal(outcome.reason.cause, original);
    }
    assert.equal(f.submissions(), 1);
    assert.equal(f.table.pendingCount, 0);
  });
}
