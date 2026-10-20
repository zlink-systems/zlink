const { prepareZLinkRequestReply, identityRequestReply } = require('../../packages/framework/dist/runtime/messaging/request-result');
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
const {
  ZLinkManagedStream
} = require('../../packages/framework/dist/runtime/streams/managed-stream');

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
    const binding = {
      sessionOwnerNodeRid: 'session-owner',
      sessionOwnerNodeGeneration: 11n,
      bindingGeneration: 7n
    };
    const stream = new ZLinkManagedStream({ async unbindActor() {} }, 'session');
    stream.actorBinding = () => binding;
    const context = {
      boundActors: [actor],
      stream,
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
        primaryMeshNode: {
          status: () => ({
            routingId: binding.sessionOwnerNodeRid,
            lifecycleGeneration: binding.sessionOwnerNodeGeneration
          })
        }
      }),
      routeTransport: {
        request(_channel, node) { return prepareZLinkRequestReply((async () => {
          sent.push(node);
        })(), identityRequestReply); },
        requestToSpot(target) { return prepareZLinkRequestReply((async () => {
          sent.push(String(target.targetNodeRid));
        })(), identityRequestReply); }
      }
    });
    const sender = new ZLinkBoundActorRelaySender(
      registry,
      {},
      {
        notifyDisconnected: (selected, signal, snapshot) =>
          relay.notifyBoundActorDisconnected(selected, signal, snapshot)
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
