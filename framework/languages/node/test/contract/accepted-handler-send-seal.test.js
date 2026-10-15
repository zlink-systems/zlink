const assert = require('node:assert/strict');
const test = require('node:test');
const { ZLinkRuntimeAdmissionGate } = require('../../packages/framework/dist/runtime/admission');
const {
  ZLinkNativeFallbackBoundSession
} = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');

for (const shutdown of [false, true]) {
  test(`accepted handler sends after ${shutdown ? 'shutdown' : 'relocation'} seal`, async () => {
    const admission = new ZLinkRuntimeAdmissionGate();
    admission.register('mesh');
    let submissions = 0;
    const session = new ZLinkNativeFallbackBoundSession({
      runtime: {
        async submitLocalBoundSession() {
          submissions++;
          return { status: ZLinkSubmitStatus.Submitted };
        }
      },
      routedTransport: {},
      actorRefProvider: () => undefined,
      nativeActorNodeProvider: () => undefined,
      localActorProvider: () => true,
      remoteBoundSessionTargetProvider: () => undefined,
      remoteActorPacketTargetProvider: () => undefined,
      actorId: 'accepted-handler',
      reportError: (error) => {
        throw error;
      }
    });
    const claim = admission.claim('mesh', 'accepted handler');
    let release;
    const latch = new Promise((resolve) => {
      release = resolve;
    });
    let started;
    const entered = new Promise((resolve) => {
      started = resolve;
    });
    const handler = (async () => {
      try {
        started();
        await latch;
        await session.send({ value: 'push' }).packetName('AcceptedPush').submit();
      } finally {
        claim.close();
      }
    })();
    try {
      await entered;
      if (shutdown) admission.close();
      else admission.seal('mesh');
      assert.equal(admission.accepts('mesh'), false);
      assert.throws(() => admission.claim('mesh', 'new send'));
      assert.throws(() => admission.claim('mesh', 'new request'));
      assert.equal(admission.pending('mesh'), 1);
      release();
      await handler;
      assert.equal(submissions, 1);
      assert.equal(admission.pending('mesh'), 0);
    } finally {
      release();
      await handler;
    }
  });
}
