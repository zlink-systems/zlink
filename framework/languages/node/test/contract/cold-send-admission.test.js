const assert = require('node:assert/strict');
const test = require('node:test');
const { setTimeout: delay } = require('node:timers/promises');
const internal = require('../../packages/framework/dist/internal');

function coldSend(defaultRequestTimeoutMs, resolve) {
  let admit;
  let submissions = 0;
  let deadline;
  const admission = new Promise((done) => {
    admit = done;
  });
  const addressTransport = new internal.ZLinkHostSpotAddressTransport({
    resolver: () => ({ resolve }),
    routed: {
      sendToSpot() {
        throw new Error('Ready route must not be used.');
      }
    },
    meshNames: () => ['play'],
    meshNode: () => ({
      instanceSpotPlacementTypes: () => ['chat-room'],
      selectObjectPlacement: () => ({
        kind: 'selected',
        target: {
          targetNodeRid: 'node-b',
          targetNodeGeneration: 7n,
          descriptorVersion: '11'
        }
      }),
      sendToMissingInstanceSpot(_target, _encoded, activationDeadline) {
        submissions += 1;
        deadline = activationDeadline;
        return admission;
      }
    }),
    completions: () => undefined,
    defaultRequestTimeoutMs
  });
  const outbound = new internal.DefaultZLinkSpotOutbound({
    serial: new internal.ZLinkSpotSerialTurnExecutor(),
    addressTransport
  });
  class Notice {}
  return {
    call: outbound.sendToSpot('instance-1', new Notice()).instanceSpot('chat-room').inMesh('play'),
    admit: () => admit(0),
    get submissions() {
      return submissions;
    },
    get deadline() {
      return deadline;
    }
  };
}

test('cold public send stays pending beyond activation deadline and completes once on envelope admission', async () => {
  const fixture = coldSend(100, async () => undefined);
  let terminals = 0;
  const pending = fixture.call.submit().finally(() => {
    terminals += 1;
  });
  // Observe rejection immediately so the old caller timer is a test failure, not an unhandled rejection.
  pending.catch(() => {});
  try {
    await delay(3100);
    assert.equal(terminals, 0);
    assert.equal(fixture.submissions, 1);
  } finally {
    fixture.admit();
    await pending;
  }
  assert.equal(terminals, 1);
  assert.equal(fixture.submissions, 1);
});

test('cold send activation deadline uses source submission start and is not renewed after resolve', async (t) => {
  let now = 1700000000000;
  t.mock.method(Date, 'now', () => now);
  let releaseResolve;
  let beganResolve;
  const resolving = new Promise((done) => {
    beganResolve = done;
  });
  const authority = new Promise((done) => {
    releaseResolve = done;
  });
  const fixture = coldSend(5000, async () => {
    beganResolve();
    await authority;
    return undefined;
  });
  const pending = fixture.call.submit();
  await resolving;
  now += 7000;
  releaseResolve();
  await delay(0);
  try {
    assert.equal(fixture.deadline, 1700000005000n);
    assert.equal(fixture.submissions, 1);
  } finally {
    fixture.admit();
    await pending;
  }
});
