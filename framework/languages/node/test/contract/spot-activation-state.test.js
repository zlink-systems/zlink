const assert = require('node:assert/strict');
const test = require('node:test');

const {
  ZLinkSpotActivation
} = require('../../packages/framework/dist/runtime/spots/spot-activation-state');

function createActivation(externalActorCount = () => 0) {
  return new ZLinkSpotActivation({
    meshName: 'play',
    spotId: 'room-1',
    spotType: class Room {},
    spot: {},
    // SpotWide execution mode drives every actor operation through this
    // shared serial, so the constructor always installs the activation's
    // execution barrier on it (spot-activation-state.ts). Production always
    // passes a real ZLinkSpotSerialExecutor; the fake only needs the one
    // method the constructor calls.
    serial: { setExecutionBarrier: () => {} },
    timers: {},
    actorHandlers: {},
    handlers: {},
    externalActorCount
  });
}

test('spot activation owns join, transfer, departure, and rejoin invariants', () => {
  const activation = createActivation();
  const actor = { context: { actorId: 'alice' } };

  activation.commitActorJoin(actor);
  assert.equal(activation.resolveJoinedActor('alice'), actor);
  assert.equal(activation.hasDepartedActor('alice'), false);
  assert.equal(activation.canClose(), false);

  activation.beginActorTransfer('alice');
  assert.equal(activation.resolveJoinedActor('alice'), actor);
  assert.equal(activation.hasDepartedActor('alice'), true);
  assert.equal(activation.canClose(), false);

  activation.cancelActorTransfer('alice');
  assert.equal(activation.resolveJoinedActor('alice'), actor);
  assert.equal(activation.hasDepartedActor('alice'), false);
  assert.equal(activation.canClose(), false);

  activation.commitActorDeparture('alice');
  assert.equal(activation.resolveJoinedActor('alice'), undefined);
  assert.equal(activation.hasDepartedActor('alice'), true);
  assert.equal(activation.canClose(), true);

  activation.commitActorJoin(actor);
  assert.equal(activation.resolveJoinedActor('alice'), actor);
  assert.equal(activation.hasDepartedActor('alice'), false);
  assert.equal(activation.canClose(), false);
});

test('spot activation failed rejoin restores the departed state', () => {
  const activation = createActivation();
  const actor = { context: { actorId: 'alice' } };

  activation.commitActorJoin(actor);
  activation.commitActorDeparture('alice');
  const rollback = activation.commitActorJoin(actor);
  rollback();

  assert.equal(activation.resolveJoinedActor('alice'), undefined);
  assert.equal(activation.hasDepartedActor('alice'), true);
  assert.equal(activation.canClose(), true);
});

test('spot activation includes external actors in its close decision', () => {
  let externalActors = 1;
  const activation = createActivation(() => externalActors);

  assert.equal(activation.canClose(), false);
  externalActors = 0;
  assert.equal(activation.canClose(), true);
});

test('explicit close ignores a stale native count only after every tracked actor departs', () => {
  const activation = createActivation(() => 1);
  const actor = { context: { actorId: 'alice' } };
  activation.commitActorJoin(actor);

  activation.requestClose();
  assert.equal(activation.canClose(), false);

  activation.commitActorDeparture(actor.context.actorId);
  assert.equal(activation.resolveJoinedActor(actor.context.actorId), undefined);
  assert.equal(activation.canClose(), true);
});
