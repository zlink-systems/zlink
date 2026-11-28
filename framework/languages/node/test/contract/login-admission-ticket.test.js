const assert = require('node:assert/strict');
const test = require('node:test');

const fixture = require('../../../../runtime/conformance/login-admission-queue-v1.json');

test('login admission fixture has 12 ticket vectors', () => {
  assert.equal(fixture.ticketVectors.length, 12);
});

for (const { name, given, ticket, expect } of fixture.ticketVectors) {
  test(`login admission ticket fixture: ${name}`, () => {
    const {
      AdmissionTickets
    } = require('../../packages/framework/dist/runtime/streams/admission-tickets');
    const currentKey = given.keyBase64Url;
    const previousKey = given.previousKeyBase64Url ?? undefined;
    if (expect.reason === 'keyTooShort') {
      assert.throws(() => new AdmissionTickets(currentKey, previousKey), /32.*64/);
      return;
    }
    const codec = new AdmissionTickets(currentKey, previousKey);
    const session = {
      nodeRid: given.nodeRidBase64Url,
      nodeLifecycleGeneration: String(given.nodeLifecycleGeneration),
      streamName: given.streamName,
      sessionRid: given.sessionRidBase64Url
    };
    const verifier = {
      nodeRid: given.verificationNodeRidBase64Url,
      nodeLifecycleGeneration: String(given.verificationNodeLifecycleGeneration),
      streamName: given.verificationStreamName,
      sessionRid: given.verificationSessionRidBase64Url
    };
    if (expect.reason === 'ok') {
      assert.equal(codec.issue(session, BigInt(given.epoch), given.expiresAtUnixMs), ticket);
    }
    if (expect.reason === 'previousKeyOk') {
      const oldCodec = new AdmissionTickets(previousKey);
      assert.equal(oldCodec.issue(session, BigInt(given.epoch), given.expiresAtUnixMs), ticket);
    }
    assert.equal(
      codec.verify(ticket, verifier, BigInt(given.verificationEpoch), given.nowUnixMs),
      expect.accept
    );
  });
}
