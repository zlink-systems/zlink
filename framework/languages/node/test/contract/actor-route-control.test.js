const assert = require('node:assert/strict');
const test = require('node:test');
const {
  ZLinkActorSessionBindingRegistry
} = require('../../packages/framework/dist/runtime/streams/actor-session-binding-registry');
const {
  DefaultZLinkSessionActor
} = require('../../packages/framework/dist/runtime/streams/session-context');
const {
  ZLinkBoundActorRelaySender
} = require('../../packages/framework/dist/runtime/streams/bound-actor-relay-sender');
const {
  ZLinkActorPacketRelay
} = require('../../packages/framework/dist/runtime/host/actor-packet-relay');

for (const physical of [false, true]) {
  test(`disconnect uses the committed binding route after replacement (physical=${physical})`, async () => {
    const registry = new ZLinkActorSessionBindingRegistry();
    const oldRef = {
      actorId: 'player',
      objectGeneration: 1n,
      nodeRid: 'old-owner',
      meshName: 'mesh'
    };
    const actor = new DefaultZLinkSessionActor({}, oldRef, 'binding');
    const context = {
      boundActors: [actor],
      stream: {},
      bindLocal() {},
      unbindLocal() {}
    };
    await registry.bind(context, actor, actor.bindingToken);
    const newRef = { ...oldRef, nodeRid: 'current-owner' };
    await registry.replace(
      await registry.route(actor.actorId),
      context,
      actor,
      actor.bindingToken,
      undefined,
      undefined,
      () => actor.updateRef(newRef)
    );
    const sent = [];
    const relay = new ZLinkActorPacketRelay({
      meshRouters: {},
      streamBindingRuntime: () => ({
        async find() {
          return actor;
        }
      }),
      actorManager: () => ({
        getState: () => ({
          spotId: 'departed-spot',
          remoteActorPacketTarget: {
            routerChannelId: 'mesh',
            targetNodeRid: 'old-owner',
            spotId: 'departed-spot'
          }
        })
      }),
      spotNodeRuntime: () => ({
        primaryMeshNode: { status: () => ({ routingId: 'session-owner' }) }
      }),
      routeTransport: {
        async request(_channel, node) {
          sent.push(node);
        },
        async requestToSpot(target) {
          sent.push(String(target.targetNodeRid));
        }
      }
    });
    const sender = new ZLinkBoundActorRelaySender(
      registry,
      {},
      {
        notifyDisconnected: (selected, signal) =>
          relay.notifyBoundActorDisconnected(selected, signal)
      }
    );
    if (physical) {
      await sender.notifyPhysicalDisconnect(context);
      await sender.notifyPhysicalDisconnect(context);
    } else await sender.notifyDisconnected(actor);
    assert.deepEqual(sent, ['current-owner']);
    assert.equal(await registry.route(actor.actorId), undefined);
  });
}
