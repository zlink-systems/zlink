const assert = require('node:assert/strict');
const test = require('node:test');

const { DefaultZLinkSpotHandlerRegistry } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkActorPacketKind,
  ZLinkSpotActorHandlerRegistryRuntime
} = require('../../packages/framework/dist/runtime/actors');
const { ZLinkSpotActorSend, ZLinkFrameworkException, ZLinkFrameworkErrorKind } = require('../../packages/framework/dist');

test('Spot addHandler projects actor metadata into the actor dispatcher registry', () => {
  class PlayerActor {}
  class JoinGameHandler {
    async handle() {}
  }
  ZLinkSpotActorSend('JoinGameMsg')(
    JoinGameHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(JoinGameHandler.prototype, 'handle')
  );
  const actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
  const registry = new DefaultZLinkSpotHandlerRegistry(actorHandlers);

  registry.addHandler(JoinGameHandler);

  assert.deepEqual(registry.snapshot(), [{
    kind: 'actorSend',
    handlerType: JoinGameHandler,
    actorType: Object,
    packetName: 'JoinGameMsg'
  }]);
  assert.equal(
    actorHandlers.resolvePacket(ZLinkActorPacketKind.Send, new PlayerActor(), 'JoinGameMsg')?.handlerType,
    JoinGameHandler
  );
});

test('repeated Spot subscription registration fails activation with InvalidOperation', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic');
  assert.throws(() => registry.addSubscribe(PublishHandler, 'mesh', 'topic'), (error) =>
    error instanceof ZLinkFrameworkException && error.kind === ZLinkFrameworkErrorKind.InvalidOperation);

  assert.deepEqual(registry.snapshot(), [
    { kind: 'subscribe', handlerType: PublishHandler, channelName: 'mesh', topic: 'topic' }
  ]);
});

test('empty Spot topic and duplicate packet fail with InvalidOperation', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();
  const invalid = (error) => error instanceof ZLinkFrameworkException &&
    error.kind === ZLinkFrameworkErrorKind.InvalidOperation;
  assert.throws(() => registry.addSubscribe(PublishHandler, 'mesh', ''), invalid);
  registry.addPacket(PublishHandler, 'packet');
  assert.throws(() => registry.addPacket(PublishHandler, 'packet'), invalid);
});

test('Spot subscriptions require channel membership and accept a registered channel', () => {
  const { ZLinkNodeRawMeshBackend } = require('../../packages/framework/dist/runtime/backend/node/node-raw-mesh-backend');
  const { ServiceStatefulRuntime } = require('../../packages/framework/dist/runtime/foundation/service-stateful-runtime');
  const backend = new ZLinkNodeRawMeshBackend('mesh', 'node', {});
  const stateful = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'node', 1n);
  backend.stateful = stateful;
  const spot = stateful.createSpot('spot');
  assert.throws(() => backend.setSpotSubscription(spot, 'unjoined-channel', 'topic'),
    (error) => error instanceof ZLinkFrameworkException && error.kind === ZLinkFrameworkErrorKind.InvalidOperation);
  backend.addChannelName('events');
  assert.doesNotThrow(() => backend.setSpotSubscription(spot, 'events', 'topic'));
});

test('different Spot subscription registrations remain independent', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic-a');
  registry.addSubscribe(PublishHandler, 'mesh', 'topic-b');
  registry.addSubscribe(PublishHandler, 'other-mesh', 'topic-a');

  assert.equal(registry.snapshot().length, 3);
});
