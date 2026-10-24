'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkMeshCompletionTable,
  closeMeshCompletion
} = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');
const {
  createZLinkRequest,
  prepareZLinkRequestReply
} = require('../../packages/framework/dist/runtime/messaging/request-result');
const {
  ZLinkBufferMessage
} = require('../../packages/framework/dist/runtime/backend/runtime-message');
const {
  currentFlowContext,
  runWithFlow
} = require('../../packages/framework/dist/runtime/diagnostics/flow-context');

function record(operationId, parts = []) {
  return {
    operationId,
    terminalResult: 0,
    failureErrno: 0,
    operationKind: 1,
    kindData: null,
    parts
  };
}
function request(
  table,
  operationId,
  { signal, admitted = false, decode = (value) => value, mapFailure } = {}
) {
  return createZLinkRequest((entry) => {
    const pending = table.submit(() => {
      if (admitted) entry.confirm();
      return operationId;
    }, signal, entry);
    return prepareZLinkRequestReply(pending, decode, mapFailure);
  });
}

// Completed public requests can outlive every dispatcher slot in the table.
// Read admission only after reuse, so a getter that retains a pooled slot fails.
for (const firstAdmitted of [false, true]) {
  test(`completed request keeps its admission after dispatcher reuse (admitted=${firstAdmitted})`, async () => {
    const table = new ZLinkMeshCompletionTable();
    const id = { high: 1n, low: 7n };
    const controller = new AbortController();
    const cause = new Error('first cancellation');
    const first = request(table, id, { signal: controller.signal, admitted: firstAdmitted });
    const firstFailure = assert.rejects(first, (error) => error === cause);
    controller.abort(cause);
    await firstFailure;
    for (let i = 0; i < 129; i++) {
      const nextId = { high: 2n, low: BigInt(i + 1) };
      const next = request(table, nextId, { admitted: !firstAdmitted });
      table.complete(record(nextId));
      assert.equal((await next).terminalResult, 0);
      await next.admitted;
    }
    const admission = first.admitted;
    if (firstAdmitted) await admission;
    else await assert.rejects(admission, (error) => error === cause);
    assert.equal(first.admitted, admission);
    table.dispose();
  });
}

for (const terminal of ['reply', 'materializeFailure', 'cancel', 'close']) {
  test(`typed delivery runs after terminal take and restores flow (${terminal})`, async () => {
    const diagnostics = [];
    const table = new ZLinkMeshCompletionTable((value) => diagnostics.push(value));
    const id = { high: 0xffff_ffff_ffff_ffffn, low: 0x8000_0000_0000_0000n };
    const flow = { flowId: 'entry-delivery', flowOrigin: 'Application' };
    const cause = new Error(terminal);
    const controller = new AbortController();
    let inTerminal = false;
    let applications = 0;
    const checkDelivery = () => {
      applications++;
      assert.equal(inTerminal, false, 'terminal callback stack has returned');
      assert.equal(table.pendingCount, 0, 'terminal identity has been removed');
      assert.equal(currentFlowContext(), flow);
    };
    const reply = runWithFlow(flow, () =>
      request(table, id, {
        signal: controller.signal,
        admitted: true,
        decode(value) {
          checkDelivery();
          return value;
        },
        mapFailure(error) {
          checkDelivery();
          return error;
        }
      })
    );
    const observed =
      terminal === 'reply' ? reply : assert.rejects(reply, (error) => error === cause);
    inTerminal = true;
    try {
      if (terminal === 'reply') table.complete(record(id));
      else if (terminal === 'materializeFailure')
        table.complete(record(id), () => {
          throw cause;
        });
      else if (terminal === 'cancel') controller.abort(cause);
      else table.dispose(cause);
      assert.equal(applications, 0);
      // Unknown and late identities never run materialization or application code.
      table.complete(record({ ...id, high: 1n }), () =>
        assert.fail('unknown identity materialized')
      );
      table.complete(record(id), () => assert.fail('late identity materialized'));
    } finally {
      inTerminal = false;
    }
    await observed;
    assert.equal(applications, 1);
    assert.equal(diagnostics.length, 2);
    assert.equal(currentFlowContext(), undefined);
    await reply.admitted;
    table.dispose();
  });
}

test('decoder reentry can reuse the identity without changing an older public admission', async (t) => {
  const table = new ZLinkMeshCompletionTable();
  const id = { high: 9n, low: 3n };
  const part = ZLinkBufferMessage.from('owned reply');
  let closes = 0;
  t.mock.method(part, 'close', () => {
    closes++;
  });
  let second;
  const first = request(table, id, {
    admitted: true,
    decode(completion) {
      try {
        assert.equal(table.pendingCount, 0);
        second = request(table, id, { admitted: true });
        assert.equal(table.pendingCount, 1);
        return completion.parts[0].getString();
      } finally {
        closeMeshCompletion(completion);
      }
    }
  });
  table.complete(record(id, [part]));
  assert.equal(await first, 'owned reply');
  assert.equal(closes, 1);
  table.complete(record(id));
  await second;
  await first.admitted;
  await second.admitted;
  assert.equal(table.pendingCount, 0);
  table.dispose();
});

test('pre-registration cancellation and submit throw preserve the same lazy admission cause', async () => {
  const table = new ZLinkMeshCompletionTable();
  for (const mode of ['alreadyAborted', 'reentrantAbort', 'submitThrow']) {
    const controller = new AbortController();
    const cause = new Error(mode);
    if (mode === 'alreadyAborted') controller.abort(cause);
    const reply = createZLinkRequest((entry) =>
      prepareZLinkRequestReply(
        table.submit(() => {
          if (mode === 'submitThrow') throw cause;
          if (mode === 'reentrantAbort') controller.abort(cause);
          return { high: 1n, low: 19n };
        }, controller.signal, entry),
        (value) => value
      )
    );
    const admission = reply.admitted;
    const admissionFailure = assert.rejects(admission, (error) => error === cause);
    await assert.rejects(reply, (error) => error === cause);
    await admissionFailure;
    assert.equal(table.pendingCount, 0);
  }
  table.dispose();
});


test('terminal delivery leaves the state lane and infrastructure execution area', async () => {
  const { ZLinkStateLane } = require('../../packages/framework/dist/runtime/execution/state-lane');
  const { runZLinkExecutionArea, currentZLinkExecutionArea } = require('../../packages/framework/dist/runtime/execution');
  const lane = new ZLinkStateLane();
  const table = new ZLinkMeshCompletionTable();
  const id = { high: 8n, low: 8n };
  const reply = request(table, id, { decode(value) {
    assert.equal(ZLinkStateLane.current, undefined);
    assert.notEqual(currentZLinkExecutionArea(), 'infrastructure');
    return value;
  } });
  await lane.run(() => runZLinkExecutionArea('infrastructure', () => table.complete(record(id))));
  await reply;
  table.dispose(); await lane.dispose();
});


test('first completion module load inside a lane does not install that owner in its decoder', () => {
  const { spawnSync } = require('node:child_process');
  const path = require('node:path');
  const root = path.resolve(__dirname, '../../packages/framework/dist/runtime');
  const result = spawnSync(process.execPath, ['-e', `
    const assert = require('node:assert/strict');
    const root = process.argv[1];
    const { ZLinkStateLane } = require(root + '/execution/state-lane');
    const lane = new ZLinkStateLane();
    (async () => {
      let execution, api, tableApi;
      await lane.run(() => {
        execution = require(root + '/execution');
        execution.runZLinkExecutionArea('infrastructure', () => {
          api = require(root + '/messaging/request-result');
          tableApi = require(root + '/backend/mesh-completion-table');
        });
      });
      const table = new tableApi.ZLinkMeshCompletionTable();
      const id = { high: 1n, low: 2n };
      const reply = api.createZLinkRequest(entry => api.prepareZLinkRequestReply(
        table.submit(() => id, undefined, entry), () => {
          assert.equal(ZLinkStateLane.current, undefined);
          assert.notEqual(execution.currentZLinkExecutionArea(), 'infrastructure');
          return 7;
        }
      ));
      table.complete({ operationId: id, terminalResult: 0, failureErrno: 0,
        operationKind: 1, kindData: null, parts: [] });
      assert.equal(await reply, 7);
      await reply.admitted;
      table.dispose();
      await lane.dispose();
    })().catch(error => { console.error(error); process.exitCode = 1; });
  `, root], { encoding: 'utf8', env: { ...process.env, NODE_TEST_CONTEXT: undefined } });
  assert.ifError(result.error);
  assert.equal(result.status, 0, result.stdout + result.stderr);
});
