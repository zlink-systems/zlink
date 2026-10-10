const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshCompletionTable } = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
const { ServiceMailbox } = require('../../packages/framework/dist/runtime/foundation/service-mailbox');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const { RequestResult } = require('../../packages/framework/dist/runtime/backend/runtime-values');

function harness(table, checkSlot = true) {
  let resolve;
  const promise = new Promise(done => { resolve = done; });
  const backend = new ZLinkNodeRawMeshBackend('completion', 'local', {});
  backend.setCompletionHandler((terminal, materialize) => table.complete(terminal, materialize));
  let ready = 0;
  backend.setReadyHandler(() => { ready++; return 0; });
  backend.runtime = {
    mailbox: new ServiceMailbox(),
    close() { this.mailbox.close(); },
    requestToNode() {
      if (checkSlot) assert.equal(table.pendingCount, 1, 'completion slot exists before transport submission');
      return { id: 7n, promise };
    }
  };
  return { backend, resolve, ready: () => ready };
}

test('registered completion reaches its caller with the infrastructure pump stopped', async t => {
  t.mock.method(globalThis, 'setTimeout', () => assert.fail('completion must not yield to a timer'));
  const table = new ZLinkMeshCompletionTable();
  const h = harness(table, false);
  let result;
  const pending = table.submit(() =>
    h.backend.requestToNode('peer', [Buffer.from('request')])
  ).then(value => { result = value; });
  h.resolve({ terminalResult: 0, failureCode: 0 });
  // Only Promise dispatch runs: neither infrastructure pumping nor timer yield is available.
  for (let i = 0; i < 8; i++) await Promise.resolve();
  assert.equal(result?.terminalResult, 0);
  assert.equal(h.ready(), 0);
  assert.equal(table.pendingCount, 0);
  await pending;
  table.dispose();
});

for (const terminal of ['cancel', 'close']) {
  test(`registered ${terminal} consumes the slot and diagnoses a late native completion`, async () => {
    const diagnostics = [];
    const table = new ZLinkMeshCompletionTable(value => diagnostics.push(value));
    const h = harness(table);
    const controller = new AbortController();
    const pending = table.submit(() =>
      h.backend.requestToNode('peer', [Buffer.from('request')]), controller.signal
    );
    const reason = new Error(terminal);
    const rejected = assert.rejects(pending, error => error === reason);
    if (terminal === 'cancel') controller.abort(reason);
    else {
      h.backend.close();
      table.dispose(reason);
    }
    await rejected;
    h.resolve({ terminalResult: 0, failureCode: 0 });
    for (let i = 0; i < 8; i++) await Promise.resolve();
    assert.equal(table.pendingCount, 0);
    assert.equal(diagnostics.length, 1);
    assert.equal(diagnostics[0].kind, 'unknownOrLate');
    assert.equal(h.ready(), 0);
    table.dispose();
  });
}

test('immediate terminal rejection uses the registered owner without infrastructure readiness', async () => {
  const table = new ZLinkMeshCompletionTable();
  const h = harness(table);
  h.backend.stateful = { registry: { binding: () => undefined } };
  const result = await table.submit(() => h.backend.closeActorBoundSession({}, 1n));
  assert.equal(result.terminalResult, RequestResult.NotFound);
  assert.equal(table.pendingCount, 0);
  assert.equal(h.ready(), 0);
  table.dispose();
});

test('raw pull completion uses the existing mailbox and releases its claim', async () => {
  const table = new ZLinkMeshCompletionTable();
  const h = harness(table, false);
  h.backend.completionHandler = undefined;
  const operation = h.backend.requestToNode('peer', [Buffer.from('request')]);
  h.resolve({ terminalResult: 0, failureCode: 0 });
  for (let i = 0; i < 8; i++) await Promise.resolve();
  const ready = h.backend.createReadyBatch(1);
  const receive = h.backend.createReceiveBatch(1, 1);
  const drained = h.backend.drainReady(ReadyDomain.Infrastructure, ready);
  assert.equal(drained.records.length, 1);
  assert.equal(drained.records[0].terminalCompletion, true);
  const claim = ready.takeClaim(0);
  try {
    const records = claim.recvBatch(receive).records;
    assert.deepEqual(records[0].operationId, operation);
    assert.equal(records[0].terminalResult, 0);
    assert.equal(h.backend.runtime.mailbox.pendingMessages('infrastructure'), 0);
  } finally {
    claim.release();
    ready.close();
    receive.close();
    table.dispose();
  }
});

test('registered materialization failure rejects its consumed slot and late results do not decode', async () => {
  const diagnostics = [];
  const table = new ZLinkMeshCompletionTable(value => diagnostics.push(value));
  const operationId = { high: 1n, low: 9n };
  const pending = table.submit(() => operationId);
  const failure = new Error('invalid multipart');
  table.complete({ operationId, terminalResult: 0 }, () => { throw failure; });
  await assert.rejects(pending, error => error === failure);
  table.complete({ operationId, terminalResult: 0 }, () => assert.fail('late completion must not decode'));
  assert.equal(table.pendingCount, 0);
  assert.equal(diagnostics.length, 1);
  table.dispose();
});

test('submission failure and cancellation release the reserved slot without removing a duplicate waiter', async () => {
  const table = new ZLinkMeshCompletionTable();
  const failure = new Error('submission failed');
  await assert.rejects(table.submit(() => { throw failure; }), error => error === failure);
  assert.equal(table.pendingCount, 0);
  const controller = new AbortController();
  await assert.rejects(table.submit(() => {
    controller.abort(failure);
    return { high: 1n, low: 10n };
  }, controller.signal), error => error === failure);
  assert.equal(table.pendingCount, 0);
  const operationId = { high: 1n, low: 11n };
  const first = table.submit(() => operationId);
  await assert.rejects(table.submit(() => operationId), /already pending/);
  assert.equal(table.pendingCount, 1);
  table.complete({ operationId, terminalResult: 0, failureErrno: 0, operationKind: 0, kindData: null, parts: [] });
  await first;
  assert.equal(table.pendingCount, 0);
  table.dispose();
});
