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
  const stateful = new ServiceStatefulRuntime({ setServiceIngress() {} }, 'node', 1n);
  const backend = new ZLinkNodeRawMeshBackend(
    'mesh', 'node', {}, undefined, undefined, undefined, undefined, { stateful }
  );
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

for (const kind of ['spotPacketHandlers', 'spotSubscriptionHandlers', 'spotActorSendHandlers', 'spotActorRequestHandlers', 'entrySpotPacketHandlers', 'entrySpotActorSendHandlers']) {
  test(`${kind} rejects a base owner at startup and accepts an exact owner`, () => {
    const { createFrameworkRegistration, ZLinkConfigurationException } = require('../../packages/framework/dist/internal');
    class BaseSpot {}
    class DerivedSpot extends BaseSpot {}
    class OwnerHandler {}
    class PlayerActor {}
    const entry = kind.startsWith('entry');
    const options = (concrete) => ({
      spotNodes: { owner: {
        router: { bind: 'tcp://127.0.0.1:0' },
        ...(entry ? { entrySpotType: concrete } : { spotFactories: [concrete] }),
        [kind]: [{
          ...(entry ? { entrySpotType: BaseSpot } : { spotType: BaseSpot }),
          handlerType: OwnerHandler, actorType: PlayerActor, packetName: 'owner-message',
          channelName: 'events', topic: 'owner-topic', messageType: Object
        }]
      }}
    });
    assert.throws(() => createFrameworkRegistration(options(DerivedSpot)), (error) => {
      assert.ok(error instanceof ZLinkConfigurationException);
      for (const type of [OwnerHandler, BaseSpot, DerivedSpot]) assert.ok(error.message.includes(type.name), error.message);
      return true;
    });
    assert.doesNotThrow(() => createFrameworkRegistration(options(BaseSpot)));
    if (!entry) {
      const both = options(BaseSpot);
      both.spotNodes.owner.spotFactories.push(DerivedSpot);
      assert.doesNotThrow(() => createFrameworkRegistration(both));
    }
  });
}

test('Nest handler scan rejects a base Spot owner through the startup validator', () => {
  require('reflect-metadata');
  const { createDiscoveredRegistration } = require('../../packages/nestjs/dist/registration-composer');
  const { zlinkSpotPacketHandler } = require('../../packages/nestjs/dist/decorators');
  const { ZLinkConfigurationException } = require('../../packages/framework/dist/internal');
  class BaseSpot {}
  class DerivedSpot extends BaseSpot {}
  class ScannedOwnerHandler { async handle() {} }
  zlinkSpotPacketHandler({ spot: () => BaseSpot, packetName: 'owner-message' })(ScannedOwnerHandler);
  const discovery = { getProviders: () => [{ token: ScannedOwnerHandler, metatype: ScannedOwnerHandler }] };
  const moduleRef = { get: () => undefined };
  const registration = (concrete) => createDiscoveredRegistration({
    spotNodes: { owner: { router: { bind: 'tcp://127.0.0.1:0' }, spotFactories: [concrete] } }
  }, discovery, moduleRef);
  assert.throws(() => registration(DerivedSpot), (error) => {
    assert.ok(error instanceof ZLinkConfigurationException);
    for (const type of [ScannedOwnerHandler, BaseSpot, DerivedSpot]) assert.ok(error.message.includes(type.name), error.message);
    return true;
  });
  assert.doesNotThrow(() => registration(BaseSpot));
  for (const sameNode of [false, true]) {
    assert.doesNotThrow(() => createDiscoveredRegistration({
      spotNodes: {
        owner: { router: { bind: 'tcp://127.0.0.1:0' }, spotFactories: sameNode ? [BaseSpot, DerivedSpot] : [BaseSpot] },
        ...(sameNode ? {} : { derived: { router: { bind: 'tcp://127.0.0.1:0' }, spotFactories: [DerivedSpot] } })
      }
    }, discovery, moduleRef));
  }
});
