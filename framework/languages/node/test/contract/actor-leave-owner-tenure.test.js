const assert = require('node:assert/strict');
const test = require('node:test');
const zlink = require('@zlink-systems/zlink');

const {
  ApplicationJobQueue,
  ServiceStatefulRegistry,
  ServiceStaleGenerationError,
  ZLinkNodeBackendAdapterFactory,
  resolveApplicationJobQueueConfiguration
} = require('../../packages/framework/dist/internal');

test('backend Leave checks the caller owner tenure before membership epoch', () => {
  const registry = new ServiceStatefulRegistry('owner');
  const actor = { actorId: 'player', generation: 5n, nodeRid: 'owner' };
  const spot = registry.createSpot('room', 'user', 'room-type').ref;
  const current = registry.restoreActor(actor, 'player-type', spot, 1n, 9n);

  assert.throws(
    () => registry.leaveActor(actor, 1n, 8n),
    /Actor 'player' owner tenure is not current/
  );
  assert.throws(
    () => registry.leaveActor(actor, 4n, 8n),
    /Actor 'player' owner tenure is not current/
  );
  assert.throws(() => registry.leaveActor(actor, 4n, 9n), ServiceStaleGenerationError);
  assert.equal(registry.actor(actor.actorId), current);

  registry.restoreActor(actor, 'player-type', spot, 4n, 9n);
  const transition = registry.leaveActor(actor, 4n, 9n);
  assert.equal(transition.previousMembershipEpoch, 4n);
  assert.equal(transition.currentMembershipEpoch, 5n);
  assert.equal(transition.previousSpot.spotId, spot.spotId);
  assert.equal(transition.currentSpot.spotId, registry.createEntrySpot().ref.spotId);
});

test('raw mesh Leave forwards the caller owner tenure to the registry', async () => {
  const factory = new ZLinkNodeBackendAdapterFactory();
  const context = factory.createChannelAdapter().createContext();
  const node = factory.createMeshAdapter().createMeshNode(context, {
    meshName: 'leave-owner-tenure',
    routingId: 'owner',
    applicationJobQueue: new ApplicationJobQueue(resolveApplicationJobQueueConfiguration({}))
  });
  try {
    node.setBind('inproc://leave-owner-tenure');
    node.start();
    const actor = node.restoreActorAuthority('player', 'player-type', 5n, 9n, undefined, 0n, 4n);
    const leave = (ownerGeneration) =>
      new Promise((resolve) => {
        node.setCompletionHandler((terminal) => resolve(terminal.terminalResult));
        node.leaveActor(actor, 4n, ownerGeneration);
      });

    assert.notEqual(await leave(8n), zlink.RequestResult.Ok);
    assert.equal(node.actorLookup(actor.actorId).membershipEpoch, 4n);
    assert.equal(await leave(9n), zlink.RequestResult.Ok);
    assert.equal(node.actorLookup(actor.actorId).membershipEpoch, 5n);
  } finally {
    await node.close();
    await context.dispose();
  }
});
