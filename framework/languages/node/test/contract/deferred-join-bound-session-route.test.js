const assert = require('node:assert/strict');
const test = require('node:test');
const framework = require('../../packages/framework/dist/internal');
const {
  ZLinkNativeFallbackBoundSession
} = require('../../packages/framework/dist/runtime/streams/native-fallback-bound-session');
const {
  ZLinkSubmitStatus
} = require('../../packages/framework/dist/runtime/messaging/submission-result');

for (const status of [ZLinkSubmitStatus.Submitted, ZLinkSubmitStatus.RouteNotConnected]) {
  test(`a relocated local Actor submits to its remote Session owner once: ${status}`, async () => {
    let nativeSubmits = 0;
    const routed = [];
    const runtime = new framework.ZLinkStreamBindingRuntime();
    const session = new ZLinkNativeFallbackBoundSession({
      runtime,
      actorId: 'relocated-player',
      actorRefProvider: () => ({
        actorId: 'relocated-player',
        objectGeneration: 5n,
        nodeRid: 'target',
        meshName: 'play',
        bindingGeneration: 6n,
        ownershipGeneration: 12n
      }),
      localActorProvider: () => true,
      nativeActorNodeProvider: () => ({
        async sendActorBoundSession() {
          nativeSubmits++;
          // Native Session routes are local to the Session owner, not the relocated Actor.
          return { status: ZLinkSubmitStatus.RouteNotConnected };
        }
      }),
      remoteBoundSessionTargetProvider: () => ({
        routerChannelId: 'play',
        targetNodeRid: 'session-owner',
        spotId: 'entry',
        sessionNodeRid: 'session-owner',
        sessionRid: 'session',
        bindingGeneration: 6n
      }),
      remoteActorPacketTargetProvider: () => undefined,
      routedTransport: {
        async submitInfrastructure(channel, rid, packet, payload) {
          routed.push({ channel, rid, packet, payload });
          return { status };
        }
      },
      reportError: (error) => {
        throw error;
      }
    });
    const send = session.send({ view: 'dungeon' }).packetName('DungeonView');
    if (status === ZLinkSubmitStatus.Submitted) await send.submit();
    else await assert.rejects(() => send.submit(), /Bound session send route is not connected/);
    assert.equal(nativeSubmits, 0, 'the stored remote binding selects the Session owner');
    assert.equal(routed.length, 1, 'transport failure is terminal without resubmission');
    assert.equal(routed[0].rid, 'session-owner');
    assert.equal(routed[0].channel, 'play');
  });
}
