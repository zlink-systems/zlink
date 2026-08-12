const assert = require('node:assert/strict');
const test = require('node:test');

const { DefaultZLinkSpotHandlerRegistry } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkActorPacketKind,
  ZLinkSpotActorHandlerRegistryRuntime
} = require('../../packages/framework/dist/runtime/actors');
const { ZLinkSpotActorSend } = require('../../packages/framework/dist');

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

test('repeated Spot subscription registration keeps one handler entry', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic');
  registry.addSubscribe(PublishHandler, 'mesh', 'topic');

  assert.deepEqual(registry.snapshot(), [
    { kind: 'subscribe', handlerType: PublishHandler, channelName: 'mesh', topic: 'topic' }
  ]);
});

test('different Spot subscription registrations remain independent', () => {
  class PublishHandler {}
  const registry = new DefaultZLinkSpotHandlerRegistry();

  registry.addSubscribe(PublishHandler, 'mesh', 'topic-a');
  registry.addSubscribe(PublishHandler, 'mesh', 'topic-b');
  registry.addSubscribe(PublishHandler, 'other-mesh', 'topic-a');

  assert.equal(registry.snapshot().length, 3);
});
