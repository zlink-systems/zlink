'use strict';
const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkHostServiceRelocationRuntime } = require('../../packages/framework/dist/runtime/host/service-relocation-host-runtime');
const { ReceiveKind } = require('../../packages/framework/dist/runtime/foundation/service-runtime-contracts');

function record(...payloads) {
  return { kind: ReceiveKind.NodeSend, parts: payloads.map(payload => ({ data: () => Buffer.from(payload) })) };
}

test('multipart ordinary headers bypass the single-part Actor terminal decoder', t => {
  const host = Object.create(ZLinkHostServiceRelocationRuntime.prototype);
  const parsed = t.mock.method(JSON, 'parse');
  for (const header of ['{"kind":3,"channelName":"bench"}', '{malformed']) {
    assert.equal(host.tryHandleControl('bench', record(header, 'body')), false);
  }
  assert.equal(parsed.mock.callCount(), 0);
});

test('Actor source leave terminals retain single-part acceptance and multipart rejection', () => {
  const host = Object.create(ZLinkHostServiceRelocationRuntime.prototype);
  let accepted = 0;
  host.acceptActorJoinSourceLeave = value => { accepted++; return value.succeeded; };
  const terminal = JSON.stringify({ packetName: '__zlink.actor.source_leave.terminal', transferId: 'x', actorId: 'a', succeeded: true });
  assert.equal(host.tryHandleControl('bench', record(terminal)), true);
  assert.equal(host.tryHandleControl('bench', record(terminal, 'body')), false);
  assert.equal(host.tryHandleControl('bench', record('{malformed')), false);
  assert.equal(host.tryHandleControl('bench', record()), false);
  assert.equal(accepted, 1);
});
