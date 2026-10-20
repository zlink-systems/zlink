'use strict';

const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkMeshDispatchPump } = require('../../packages/framework/dist/runtime/backend/mesh-dispatch-pump');
const { ReadyDomain } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');
const wire = require('../../packages/framework/dist/runtime/foundation/service-wire-m6a-codec');

function countedArray(values, counter) {
  Object.defineProperty(values, Symbol.iterator, {
    value() {
      counter.iterators += 1;
      return Array.prototype[Symbol.iterator].call(this);
    }
  });
  return values;
}

test('multipart encode reads parts without iterators and preserves wire bytes and ownership', () => {
  const counter = { iterators: 0 };
  const parts = countedArray([Buffer.from('a'), new Uint8Array(0), Buffer.from('bc')], counter);
  const encoded = wire.encodeMultipartApplicationPayload(parts, 'P', 'c');
  const expected = wire.encodeApplicationPayload({
    packetName: 'P',
    contentType: 'c',
    payload: Buffer.from('00000003000000016100000000000000026263', 'hex')
  });
  assert.deepEqual(encoded, expected);
  assert.equal(counter.iterators, 0);
  parts[0][0] = 0;
  parts[2][0] = 0;
  assert.deepEqual(encoded, expected);
  assert.throws(() => wire.encodeMultipartApplicationPayload([], 'P', 'c'), wire.ServiceWireProtocolError);
});

for (const outcome of ['success', 'rejection', 'shutdown']) {
  test(`mesh drain preserves async dispatch and cleanup order without array iterators: ${outcome}`, async () => {
    const counter = { iterators: 0 };
    const events = [];
    const failure = new Error('handler rejected');
    let resume;
    const suspended = new Promise((resolve) => { resume = resolve; });
    const records = countedArray([0, 1, 2].map((id) => ({
      id,
      parts: countedArray([0, 1].map((part) => ({ close() { events.push(`close:${id}:${part}`); } })), counter),
      releaseRetainedIngress() { events.push(`release:${id}`); },
      onTerminalCompletion() { events.push(`terminal:${id}`); }
    })), counter);
    const pump = new ZLinkMeshDispatchPump({}, {
      applicationJobQueue: { acquire() { throw new Error('infrastructure must not acquire capacity'); } },
      monotonicNowMs: () => 0,
      dispatch(_owner, record) {
        events.push(`dispatch:${record.id}`);
        if (record.id === 0) return suspended.then(() => {
          if (outcome === 'rejection') throw failure;
        });
      },
      reportError(error) { assert.strictEqual(error, failure); events.push('error:0'); }
    });
    const claim = {
      recvBatch() { return { ok: true, records }; },
      release() { events.push('claim:release'); }
    };
    const receiveBatch = { reset() {} };
    const pending = pump.drainClaim(ReadyDomain.Infrastructure, {}, claim, receiveBatch);
    assert.deepEqual(events, ['dispatch:0']);
    if (outcome === 'shutdown') await pump.dispose();
    resume();
    await pending;
    const dispatch = outcome === 'shutdown'
      ? ['dispatch:0', 'terminal:0']
      : ['dispatch:0', outcome === 'rejection' ? 'error:0' : 'terminal:0',
        'dispatch:1', 'terminal:1', 'dispatch:2', 'terminal:2'];
    assert.deepEqual(events, [...dispatch,
      'close:0:0', 'close:0:1', 'release:0',
      'close:1:0', 'close:1:1', 'release:1',
      'close:2:0', 'close:2:1', 'release:2', 'claim:release']);
    assert.equal(counter.iterators, 0);
  });
}
