'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshDispatchPump } = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

for (const domain of [ReadyDomain.Application, ReadyDomain.Infrastructure]) {
  test(`single synchronous claim returns no Promise: domain ${domain}`, () => {
    const events = [];
    const pump = new ZLinkMeshDispatchPump({}, {
      applicationJobQueue: {},
      dispatch() { events.push('dispatch'); }
    });
    const record = {
      parts: [{ close() { events.push('part'); } }],
      onTerminalCompletion() { events.push('terminal'); },
      releaseRetainedIngress() { events.push('retained'); }
    };
    const pending = pump.dispatchClaim(domain, { terminalCompletion: true }, {
      recvBatch() { return { ok: true, records: [record] }; },
      release() { events.push('claim'); }
    }, pump.createClaimDrain({ reset() {} }));
    assert.equal(pending, undefined);
    assert.deepEqual(events, ['dispatch', 'terminal', 'part', 'retained', 'claim']);
  });
}

test('single claim retains ownership through pending dispatch and terminal completion', async () => {
  const events = [];
  let dispatchDone;
  let terminalDone;
  const dispatch = new Promise((resolve) => { dispatchDone = resolve; });
  const terminal = new Promise((resolve) => { terminalDone = resolve; });
  const pump = new ZLinkMeshDispatchPump({}, {
    applicationJobQueue: {},
    dispatch() { events.push('dispatch'); return dispatch; }
  });
  const pending = pump.dispatchClaim(ReadyDomain.Infrastructure, {}, {
    recvBatch() {
      return { ok: true, records: [{
        parts: [{ close() { events.push('part'); } }],
        onTerminalCompletion() { events.push('terminal'); return terminal; }
      }] };
    },
    release() { events.push('claim'); }
  }, pump.createClaimDrain({ reset() {} }));
  assert.ok(pending instanceof Promise);
  assert.deepEqual(events, ['dispatch']);
  dispatchDone();
  await new Promise(setImmediate);
  assert.deepEqual(events, ['dispatch', 'terminal']);
  terminalDone();
  await pending;
  assert.deepEqual(events, ['dispatch', 'terminal', 'part', 'claim']);
});

test('empty ordinary receive returns its unused permit before releasing the claim', async () => {
  const events = [];
  const pump = new ZLinkMeshDispatchPump({}, {
    applicationJobQueue: {
      acquire() {
        events.push('permit');
        return Promise.resolve({ releaseAfterInternalProcessing() { events.push('permit-release'); } });
      }
    },
    dispatch() { assert.fail('empty receive must not dispatch'); }
  });
  const pending = pump.dispatchClaim(ReadyDomain.Application, {}, {
    recvBatch() { events.push('receive'); return { ok: false, records: [] }; },
    release() { events.push('claim'); }
  }, pump.createClaimDrain({ reset() {} }));
  assert.ok(pending instanceof Promise);
  await pending;
  assert.deepEqual(events, ['permit', 'receive', 'permit-release', 'claim']);
});

test('worker claim state releases suspended ownership before reuse and empty receive', async () => {
  const events = [];
  let resume;
  const suspended = new Promise((resolve) => { resume = resolve; });
  const pump = new ZLinkMeshDispatchPump({}, {
    applicationJobQueue: {},
    dispatch() { return suspended; }
  });
  const batch = { reset() {}, close() { assert.fail('worker owns its receive batch'); } };
  const state = pump.createClaimDrain(batch);
  const owner = { ordinaryIngressPreAdmitted: true };
  const record = {
    parts: [{ close() { events.push('part'); } }],
    applicationJobPermit: { releaseAfterInternalProcessing() { events.push('permit'); } },
    onTerminalCompletion() { events.push('terminal'); }
  };
  const first = pump.dispatchClaim(ReadyDomain.Application, owner, {
    recvBatch() { return { ok: true, records: [record] }; },
    release() { events.push('first'); }
  }, state);
  assert.ok(first instanceof Promise);
  assert.strictEqual(state.records[0], record);
  resume();
  await first;
  for (const key of ['records', 'owner', 'claim', 'claimPermit']) assert.equal(state[key], undefined);
  const second = pump.dispatchClaim(ReadyDomain.Application, owner, {
    recvBatch() { return { ok: false, records: [] }; },
    release() { events.push('second'); }
  }, state);
  assert.equal(second, undefined);
  assert.deepEqual(events, ['terminal', 'permit', 'part', 'first', 'second']);
});
