'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');

const framework = require('../../packages/framework/dist/internal');
const completion = require('../../packages/framework/dist/runtime/backend/mesh-completion-table');

const fixture = JSON.parse(fs.readFileSync(path.resolve(
  __dirname,
  '../../../../runtime/conformance/completion-terminal-v1.json'
), 'utf8'));

function operation(name) {
  const value = fixture.operations[name];
  return {
    high: BigInt(value.operationId.high),
    low: BigInt(value.operationId.low)
  };
}

function record(operationId, terminal) {
  return {
    operationId,
    terminalResult: terminal === 'reply:protocolError' ? 3 : 0,
    failureErrno: 0,
    operationKind: 0,
    kindData: null,
    parts: []
  };
}

test('mesh completion table consumes the shared identity and terminal fixture', async () => {
  assert.equal(fixture.fixture, 'zlink.framework.completion-terminal');
  assert.equal(
    completion.ZLINK_MESH_COMPLETION_CAPACITY,
    fixture.limits.pendingOperationCapacity
  );

  const alpha = operation('alpha');
  const gamma = operation('gamma');
  assert.equal(alpha.low, gamma.low);
  assert.notEqual(alpha.high, gamma.high);
  const identityTable = new completion.ZLinkMeshCompletionTable();
  const alphaPending = identityTable.submit(() => alpha);
  const gammaPending = identityTable.submit(() => gamma);
  identityTable.complete(record(gamma, 'reply:success'));
  identityTable.complete(record(alpha, 'reply:success'));
  assert.equal((await alphaPending).terminalResult, 0);
  assert.equal((await gammaPending).terminalResult, 0);
  identityTable.dispose();

  for (const scenario of fixture.raceScenarios) {
    const diagnostics = [];
    const table = new completion.ZLinkMeshCompletionTable(
      fixture.limits.pendingOperationCapacity,
      (diagnostic) => diagnostics.push(diagnostic)
    );
    const operationId = operation(scenario.operation);
    const controller = new AbortController();
    let applicationCompletionCount = 0;
    const pending = scenario.registered === false
      ? undefined
      : table.submit(() => operationId, controller.signal).then(
        (value) => {
          applicationCompletionCount += 1;
          return { kind: 'reply', value };
        },
        (error) => {
          applicationCompletionCount += 1;
          return { kind: 'error', error };
        }
      );

    for (const event of scenario.events) {
      if (event.startsWith('reply:')) {
        table.complete(record(operationId, event));
      } else if (event === 'close') {
        table.dispose(new Error('close'));
      } else {
        controller.abort(new Error(event));
      }
    }
    if (pending !== undefined) await pending;
    assert.equal(
      applicationCompletionCount,
      scenario.applicationCompletionCount,
      scenario.name
    );
    assert.equal(
      diagnostics.length,
      scenario.events.filter((event, index) =>
        event.startsWith('reply:')
        && (scenario.registered === false || index > 0)
      ).length,
      `${scenario.name}:diagnostics`
    );
    table.dispose();
  }
});

test('mesh completion capacity rejects before starting another backend operation', async () => {
  const table = new completion.ZLinkMeshCompletionTable(1);
  const first = table.submit(() => operation('alpha'));
  let started = false;
  await assert.rejects(
    table.submit(() => {
      started = true;
      return operation('beta');
    }),
    (error) => error instanceof framework.ZLinkFrameworkException
      && error.kind === framework.ZLinkFrameworkErrorKind.CapacityExceeded
  );
  assert.equal(started, false);
  assert.equal(table.pendingCount, 1);
  table.complete(record(operation('alpha'), 'reply:success'));
  await first;
  table.dispose();
});

test('mesh completion dispatch occurs after the atomic entry take', async () => {
  const table = new completion.ZLinkMeshCompletionTable();
  let callbackObserved = false;
  const pending = table.submit(() => operation('alpha')).then(() => {
    callbackObserved = true;
    assert.equal(table.pendingCount, 0);
  });
  table.complete(record(operation('alpha'), 'reply:success'));
  assert.equal(callbackObserved, false);
  await pending;
  table.dispose();
});
