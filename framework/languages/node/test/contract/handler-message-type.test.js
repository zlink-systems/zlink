require('reflect-metadata');

const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const test = require('node:test');
const { Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const {
  createFrameworkOptions
} = require('../../packages/framework/dist/contracts/Configuration/RegistrationBuilders');
const nestjs = require('../../packages/nestjs/dist');
const connector = import('@zlink-systems/stream-connector');
const { DefaultZLinkSpotHandlerRegistry } = require('../../packages/framework/dist/runtime/spots');
const {
  ZLinkSpotActorHandlerRegistryRuntime,
  ZLinkActorPacketKind
} = require('../../packages/framework/dist/runtime/actors');
const {
  ZLinkChannelDispatchPipeline
} = require('../../packages/framework/dist/runtime/channels/channel-dispatch-pipeline');
const {
  collectRouteChannelHandlers
} = require('../../packages/framework/dist/runtime/channels/channel-dispatchers');
const {
  decodeChannelPayload
} = require('../../packages/framework/dist/runtime/channels/channel-envelope');
const {
  resolveMessageTypeForDecode
} = require('../../packages/framework/dist/contracts/Common/HandlerMessageType');
const {
  ZLinkDispatchErrorSurface,
  ZLinkDispatchMessageKind,
  ZLinkRuntimeDispatchErrorReason
} = require('../../packages/framework/dist/contracts/Dispatch/ZLinkDispatchOptions');
const {
  createManualPublishHandlers,
  createDiscoveredPublishHandlers
} = require('../../packages/nestjs/dist/handler-adapters');
const {
  ZLinkInMemoryProviderLocationStore
} = require('../../packages/framework/dist/runtime/locations/in-memory-provider-location-store');
const { createBrowserSafeStreamApp } = require('./helpers/browser-safe-stream-app');

test('ZLinkMessageTypeResolver is exported beside Type from the public contracts', () => {
  const commonTypes = fs.readFileSync(
    path.join(__dirname, '../../packages/framework/dist/contracts/Common/CoreTypes.d.ts'),
    'utf8'
  );
  const commonIndex = fs.readFileSync(
    path.join(__dirname, '../../packages/framework/dist/contracts/Common/index.d.ts'),
    'utf8'
  );
  assert.match(
    commonTypes,
    /export type ZLinkMessageTypeResolver<T = unknown> = Type<T> \| \(\(\) => Type<T>\);/
  );
  assert.match(commonIndex, /ZLinkMessageTypeResolver/);
});

test('Framework and Nest registration descriptors retain their handler message type', () => {
  class ChannelMessage {}
  class ChannelHandler {
    static messageType = ChannelMessage;
  }
  class MeshChannelMessage {}
  class MeshChannelHandler {
    static messageType = MeshChannelMessage;
  }
  class RouteMessage {}
  class RouteHandler {
    static messageType = RouteMessage;
  }

  const options = createFrameworkOptions((builder) => {
    builder.addClientServerChannel('typed').server().addSendHandler(ChannelHandler);
    const mesh = builder.addRouteMesh('typed-mesh');
    mesh.channel('typed-route-channel').server().addSendHandler(MeshChannelHandler);
    mesh.addRouteSendHandler(RouteHandler);
  });
  assert.equal(options.channels.typed.sendHandlers[0].messageType, ChannelMessage);
  assert.equal(
    options.spotNodes['typed-mesh'].meshChannels['typed-route-channel'].sendHandlers[0].messageType,
    MeshChannelMessage
  );
  assert.equal(options.spotNodes['typed-mesh'].routeSendHandlers[0].messageType, RouteMessage);

  class NestPublishMessage {}
  class ManualNestPublishHandler {
    static messageType = NestPublishMessage;
  }
  const manual = createManualPublishHandlers(
    [{ packetName: 'nest.manual', handlerType: ManualNestPublishHandler }],
    {},
    new Map()
  );
  assert.equal(manual[0].messageType, NestPublishMessage);

  class DiscoveredNestPublishHandler {
    static messageType = NestPublishMessage;
  }
  const discovered = createDiscoveredPublishHandlers(
    [
      {
        handlerKey: DiscoveredNestPublishHandler,
        handlerName: 'DiscoveredNestPublishHandler',
        metadata: {
          kind: 'publish',
          groupName: 'publishers',
          packetName: 'nest.discovered',
          methodName: 'handle'
        }
      }
    ],
    ['publishers'],
    {},
    new Map()
  );
  assert.equal(discovered[0].messageType, NestPublishMessage);
});

test('Nest descriptors omit messageType when a handler does not declare it', () => {
  const {
    SpotNodeHandlerRegistry
  } = require('../../packages/nestjs/dist/spot-node-handler-registry');
  class Handler {}
  class Spot {}
  const registry = new SpotNodeHandlerRegistry({ game: {} });
  const node = registry.toOptions().game;
  registry.addSpotPacket(node, { spotType: Spot, handlerType: Handler, packetName: 'untyped' });
  assert.equal(Object.hasOwn(node.spotPacketHandlers[0], 'messageType'), false);
  assert.equal(node.spotPacketHandlers[0].handlerType, Handler);
});

test('RouteChannel keeps the declared type and original handler receiver', () => {
  class Message {}
  class Handler {
    #value = 'original receiver';
    calls = 0;

    handle() {
      this.calls += 1;
      return this.#value;
    }

    readCalls() {
      return this.calls;
    }
  }
  const routeHandler = new Handler();
  const registrations = collectRouteChannelHandlers({
    routerChannelId: 'typed-route',
    sendHandlers: [{ packetName: 'typed.packet', handler: routeHandler, messageType: Message }]
  });

  assert.equal(registrations[0].messageType, Message);
  assert.equal(registrations[0].handler, routeHandler);
  assert.equal(Object.hasOwn(routeHandler, 'messageType'), false);
  assert.equal(registrations[0].handler.handle(), 'original receiver');
  assert.equal(registrations[0].handler.readCalls(), 1);
  assert.equal(routeHandler.readCalls(), 1);
});

test('Spot packet, subscription, and Actor descriptors retain registration-scoped types', () => {
  class PacketA {}
  class PacketB {}
  class PacketHandlerA {
    static messageType = PacketA;
  }
  class PacketHandlerB {
    static messageType = PacketB;
  }
  class SubscriptionHandler {
    static messageType = PacketA;
  }
  class ActorHandler {
    static messageType = PacketB;
    async handle() {}
  }
  framework.ZLinkSpotActorSend('shared.packet')(
    ActorHandler.prototype,
    'handle',
    Object.getOwnPropertyDescriptor(ActorHandler.prototype, 'handle')
  );

  const ownerA = new DefaultZLinkSpotHandlerRegistry();
  const ownerB = new DefaultZLinkSpotHandlerRegistry();
  ownerA.addPacket(PacketHandlerA, 'shared.packet');
  ownerB.addPacket(PacketHandlerB, 'shared.packet');
  ownerA.addSubscribe(SubscriptionHandler, 'events', 'shared');
  const actorHandlers = new ZLinkSpotActorHandlerRegistryRuntime();
  const actorRegistry = new DefaultZLinkSpotHandlerRegistry(actorHandlers);
  actorRegistry.addHandler(ActorHandler, 'shared.packet');

  assert.equal(ownerA.snapshot()[0].messageType, PacketA);
  assert.equal(ownerB.snapshot()[0].messageType, PacketB);
  const packetEnvelope = {
    header: { contentType: 'application/json', channelName: 'mesh', messageName: 'shared.packet' },
    payload: Buffer.from('{"value":"owner-scoped"}')
  };
  assert.ok(
    decodeChannelPayload(packetEnvelope, undefined, ownerA.snapshot()[0].messageType) instanceof
      PacketA
  );
  assert.ok(
    decodeChannelPayload(packetEnvelope, undefined, ownerB.snapshot()[0].messageType) instanceof
      PacketB
  );
  assert.equal(ownerA.snapshot()[1].messageType, PacketA);
  assert.equal(
    actorHandlers.resolvePacket(ZLinkActorPacketKind.Send, {}, 'shared.packet').messageType,
    PacketB
  );

  class UntypedHandler {}
  const untypedRegistry = new DefaultZLinkSpotHandlerRegistry();
  untypedRegistry.addPacket(UntypedHandler, 'untyped.packet');
  assert.equal(Object.hasOwn(untypedRegistry.snapshot()[0], 'messageType'), false);
});

test('lazy handler message type resolves on decode demand and is cached', () => {
  class Message {}
  let resolverCalls = 0;
  class Handler {
    static messageType = () => {
      resolverCalls += 1;
      return Message;
    };
  }
  const registry = new DefaultZLinkSpotHandlerRegistry();
  registry.addPacket(Handler, 'lazy.packet');
  const resolver = registry.snapshot()[0].messageType;

  assert.equal(resolverCalls, 0);
  assert.equal(resolver(), Message);
  assert.equal(resolver(), Message);
  assert.equal(resolverCalls, 1);
});

test('ordinary function message type resolvers are deferred and cached', () => {
  class Message {
    constructor() {
      throw new Error('The DTO constructor must not run.');
    }
  }
  let calls = 0;
  class Handler {
    static messageType = function () {
      calls += 1;
      return Message;
    };
  }
  const registry = new DefaultZLinkSpotHandlerRegistry();
  registry.addPacket(Handler, 'ordinary.lazy');
  const declared = registry.snapshot()[0].messageType;
  assert.equal(calls, 0);
  for (let i = 0; i < 2; i += 1) {
    const value = decodeChannelPayload(
      { header: { contentType: 'application/json' }, payload: Buffer.from('{"value":1}') },
      undefined,
      resolveMessageTypeForDecode(declared)
    );
    assert.ok(value instanceof Message);
    assert.equal(value.value, 1);
  }
  assert.equal(calls, 1);
});

test('common channel decode restores JSON prototypes without constructors and passes custom types once', () => {
  let constructorCalls = 0;
  class RequiredDto {
    constructor(value) {
      constructorCalls += 1;
      if (value !== 'constructed') throw new Error('RequiredDto constructor requires a value.');
      this.value = value;
    }
  }
  new RequiredDto('constructed');
  const beforeDecode = constructorCalls;
  const envelope = (contentType, payload) => ({
    header: { contentType, channelName: 'mesh', messageName: 'typed.packet' },
    payload: Buffer.from(payload)
  });

  const decoded = decodeChannelPayload(
    envelope('application/json', '{"value":"wire"}'),
    undefined,
    RequiredDto
  );
  assert.ok(decoded instanceof RequiredDto);
  assert.equal(decoded.value, 'wire');
  assert.equal(constructorCalls, beforeDecode);
  assert.doesNotMatch(
    [
      'contracts/Common/ZLinkMessage.ts',
      'runtime/channels/channel-envelope.ts',
      'runtime/messaging/payload-codec.ts'
    ]
      .map((file) =>
        fs.readFileSync(path.join(__dirname, '../../packages/framework/src', file), 'utf8')
      )
      .join('\n'),
    /Object\s*\.\s*setPrototypeOf|(?:\.\s*__proto__|\[['"]__proto__['"]\])\s*=/
  );

  const calls = [];
  const serializer = {
    deserialize(payload, messageType) {
      calls.push({ payload, messageType });
      return { serialized: true };
    }
  };
  const serializers = { serializers: new Map([['application/x-test', serializer]]) };
  assert.deepEqual(
    decodeChannelPayload(envelope('application/x-test', 'wire'), serializers, RequiredDto),
    { serialized: true }
  );
  assert.equal(calls.length, 1);
  assert.equal(calls[0].messageType, RequiredDto);

  decodeChannelPayload(envelope('application/x-test', 'wire'), serializers);
  assert.equal(calls.length, 2);
  assert.equal(calls[1].messageType, Object);

  const structural = decodeChannelPayload(
    envelope('application/json', '{"value":"structural"}'),
    undefined
  );
  assert.equal(structural.constructor, Object);
  assert.equal(structural.value, 'structural');
});

test('malformed JSON and unsupported content types remain decode errors', () => {
  const envelope = (contentType, payload) => ({
    header: { contentType, channelName: 'mesh', messageName: 'typed.packet' },
    payload: Buffer.from(payload)
  });
  assert.throws(
    () => decodeChannelPayload(envelope('application/json', '{broken'), undefined),
    /PayloadDecodeFailed/
  );
  assert.throws(
    () => decodeChannelPayload(envelope('text/plain', 'body'), undefined),
    /unsupported channel content type/
  );
});

test('malformed JSON and unsupported content types do not reach channel handlers', async () => {
  let handlerCalls = 0;
  const reports = [];
  const dispatchErrors = {
    flow: {
      flowCreationEnabled: () => false,
      begin: () => undefined
    },
    report: (record) => reports.push(record)
  };
  const pipeline = new ZLinkChannelDispatchPipeline({
    dispatchErrors,
    surface: ZLinkDispatchErrorSurface.Channel,
    channelName: 'typed-channel'
  });
  const dispatch = (contentType, payload) =>
    pipeline.dispatchOneWay({
      fields: {
        messageKind: ZLinkDispatchMessageKind.Send,
        packetName: 'typed.packet'
      },
      envelope: {
        header: {
          contentType,
          channelName: 'typed-channel',
          messageName: 'typed.packet'
        },
        payload: Buffer.from(payload)
      },
      handler: {
        handle() {
          handlerCalls += 1;
        }
      },
      context: {
        channelName: 'typed-channel',
        contentType,
        packetName: 'typed.packet',
        metadata: new Map()
      }
    });

  await dispatch('application/json', '{broken');
  await dispatch('text/plain', 'body');

  assert.equal(handlerCalls, 0);
  assert.deepEqual(
    reports.map((record) => record.reason),
    [
      ZLinkRuntimeDispatchErrorReason.PayloadDecodeFailed,
      ZLinkRuntimeDispatchErrorReason.PayloadDecodeFailed
    ]
  );
});

test('Channel dispatch pipeline decodes with the registered message type', async () => {
  class RequiredMessage {
    constructor(value) {
      if (value === undefined) throw new Error('RequiredMessage needs a value.');
      this.value = value;
    }
  }
  const dispatchErrors = {
    flow: {
      flowCreationEnabled: () => false,
      begin: () => undefined
    },
    report() {}
  };
  const pipeline = new ZLinkChannelDispatchPipeline({
    dispatchErrors,
    surface: ZLinkDispatchErrorSurface.Channel,
    channelName: 'typed-channel'
  });
  let received;
  await pipeline.dispatchOneWay({
    fields: {
      messageKind: ZLinkDispatchMessageKind.Send,
      packetName: 'typed.packet'
    },
    envelope: {
      header: {
        contentType: 'application/json',
        channelName: 'typed-channel',
        messageName: 'typed.packet'
      },
      payload: Buffer.from('{"value":"registered"}')
    },
    messageType: RequiredMessage,
    handler: {
      handle(payload) {
        received = payload;
      }
    },
    context: {
      channelName: 'typed-channel',
      contentType: 'application/json',
      packetName: 'typed.packet',
      metadata: new Map()
    }
  });

  assert.ok(received instanceof RequiredMessage);
  assert.equal(received.value, 'registered');
});

test(
  'typed User Spot Actor send reaches a bound STREAM session with the declared packet name',
  { timeout: 30_000 },
  async () => {
    const streamConnector = await connector;
    let spotId;
    let actorRef;
    let joinedResolve;
    let relayResolve;
    let entryMessageTyped = false;
    let joinMessageTyped = false;
    let requestMessageTyped = false;
    let relayMessageTyped = false;
    let relayConstructorCalls;
    let relayResolverCalls = 0;
    const joined = new Promise((resolve) => {
      joinedResolve = resolve;
    });
    let finishRelay;
    const relayCompleted = new Promise((resolve) => {
      finishRelay = resolve;
    });

    class Player {
      constructor(context) {
        this.context = context;
      }
    }
    class PlayerFactory {
      async create(context) {
        return new Player(context);
      }
    }
    class JoinRoom {
      constructor(id) {
        this.id = id;
      }
    }
    framework.ZLinkPacket('join-room')(JoinRoom);
    class EntryQuery {
      constructor(value) {
        this.value = value;
      }
    }
    framework.ZLinkPacket('entry.query')(EntryQuery);
    class TableViewPayload {
      static constructorCalls = 0;
      constructor(value) {
        TableViewPayload.constructorCalls += 1;
        if (value === undefined) throw new Error('TableViewPayload constructor requires a value.');
        this.value = value;
      }
    }
    framework.ZLinkPacket('wire.table-view')(TableViewPayload);
    class TableQuery {
      constructor(value) {
        this.value = value;
      }
    }
    framework.ZLinkPacket('table.query')(TableQuery);

    class JoinHandler {
      static messageType = JoinRoom;
      async handle(_entry, actor, _context, message) {
        joinMessageTyped = message instanceof JoinRoom;
        actor.context.joinSpot(message.id).defer();
      }
    }
    class EntryQueryHandler {
      static messageType = EntryQuery;
      async handle(_entry, _actor, _context, message) {
        entryMessageTyped = message instanceof EntryQuery;
        return { value: message.value };
      }
    }
    class RelayHandler {
      static messageType = () => {
        relayResolverCalls += 1;
        return TableViewPayload;
      };
      async handle(_spot, actor, _context, message) {
        relayMessageTyped = message instanceof TableViewPayload;
        relayConstructorCalls = TableViewPayload.constructorCalls;
        try {
          await actor.context.boundSession.send(message).submit();
          finishRelay({ sent: true });
        } catch (error) {
          finishRelay({ sent: false, error });
          throw error;
        }
      }
    }
    class TableQueryHandler {
      static messageType = TableQuery;
      async handle(_spot, _actor, _context, message) {
        requestMessageTyped = message instanceof TableQuery;
        return { value: message.value };
      }
    }
    class EntrySpot {
      configure() {
        this.context.handlers.addHandler(JoinHandler, 'join-room');
        this.context.handlers.addHandler(EntryQueryHandler, 'entry.query');
      }
      async onCreateActor() {
        return { accepted: true };
      }
    }
    class RoomSpot {
      configure() {
        this.context.handlers.addHandler(RelayHandler, 'wire.table-view');
        this.context.handlers.addHandler(TableQueryHandler, 'table.query');
      }
      async onActorJoin() {
        return { accepted: true };
      }
      async onJoinedActor() {
        joinedResolve();
      }
    }
    class SessionFactory {
      async create(context) {
        return {
          context,
          async onDispatch(dispatch) {
            if (dispatch.packetName !== 'Bind') return;
            await context.actors.bindOrGet(actorRef);
            await context.client.reply({ bound: true }).submit();
          }
        };
      }
    }

    nestjs.zlinkEntrySpotActorSendHandler({
      entrySpot: EntrySpot,
      actor: Player,
      packetName: 'join-room'
    })(JoinHandler);
    nestjs.zlinkEntrySpotActorRequestHandler({
      entrySpot: EntrySpot,
      actor: Player,
      packetName: 'entry.query'
    })(EntryQueryHandler);
    nestjs.zlinkSpotActorSendHandler({
      spot: RoomSpot,
      actor: Player,
      packetName: 'wire.table-view'
    })(RelayHandler);
    nestjs.zlinkSpotActorRequestHandler({
      spot: RoomSpot,
      actor: Player,
      packetName: 'table.query'
    })(TableQueryHandler);

    const builder = nestjs
      .zlinkFramework()
      .addLocationStore(new ZLinkInMemoryProviderLocationStore());
    builder.configureDispatch().messageFlow('normal');
    const meshObjects = builder.addRouteMesh('typed-mesh').listen('tcp://127.0.0.1:0').objects();
    meshObjects.client();
    meshObjects
      .server()
      .addEntrySpot(EntrySpot, 'Player')
      .addSpotFactory('Room', RoomSpot, (factory) => factory.disableRelocation())
      .addActorFactory('Player', PlayerFactory, (factory) => factory.disableRelocation());
    builder
      .addStreamNode('typed-sessions')
      .enableActorDispatch()
      .bind('ws://127.0.0.1:*')
      .registerSession(SessionFactory);
    class App {}
    Module({
      imports: [nestjs.ZLinkModule.forRoot(builder.build())],
      providers: [
        EntrySpot,
        JoinHandler,
        EntryQueryHandler,
        RoomSpot,
        RelayHandler,
        TableQueryHandler,
        PlayerFactory,
        SessionFactory
      ]
    })(App);

    let app;
    let client;
    try {
      app = await createBrowserSafeStreamApp(
        () => NestFactory.createApplicationContext(App, { logger: false, abortOnError: false }),
        'typed-sessions'
      );
      const spot = await app
        .get(nestjs.ZLINK_SPOT_MANAGER)
        .create('Room')
        .inMesh('typed-mesh')
        .submit();
      spotId = spot.spot.spotId;
      const actors = app.get(nestjs.ZLINK_ACTOR_MANAGER);
      await actors.create('player', 'Player').inMesh('typed-mesh').request({}).submit();
      actorRef = await actors.find('player');
      assert.ok(actorRef);
      const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT);

      assert.deepEqual(await actorClient.requestToActor('player', new EntryQuery(11)).submit(), {
        value: 11
      });
      const streamEndpoint = app
        .get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
        .getListenerStatus('stream', 'typed-sessions').endpoint;
      client = streamConnector.zlinkStreamConnectorFactory.create({
        endpoint: streamEndpoint,
        dispatchMode: streamConnector.ZlinkStreamDispatchMode.Immediate
      });
      await client.connect();
      assert.deepEqual(await client.request({}).packetName('Bind').submit(), { bound: true });

      await actorClient.sendToActor('player', new JoinRoom(String(spotId))).submit();
      await Promise.race([
        joined,
        new Promise((_, reject) =>
          setTimeout(() => reject(new Error('Room join did not complete.')), 5000)
        )
      ]);

      assert.deepEqual(
        await actorClient.requestToActor('player', new TableQuery('state')).submit(),
        { value: 'state' }
      );
      const firstDelivery = client.waitFor('wire.table-view').timeout(5000).submit();
      await actorClient.sendToActor('player', new TableViewPayload(42)).submit();
      assert.deepEqual(await relayCompleted, { sent: true });
      const firstMessage = await firstDelivery;
      assert.equal(firstMessage.name, 'wire.table-view');
      assert.deepEqual(firstMessage.payload, { value: 42 });
      assert.equal(relayMessageTyped, true);
      assert.equal(relayConstructorCalls, 1);
      assert.equal(TableViewPayload.constructorCalls, 1);
      assert.equal(relayResolverCalls, 1);
      assert.equal(joinMessageTyped, true);
      assert.equal(entryMessageTyped, true);
      assert.equal(requestMessageTyped, true);

      let finishSecondRelay;
      const secondRelay = new Promise((resolve) => {
        finishSecondRelay = resolve;
      });
      const originalHandle = RelayHandler.prototype.handle;
      RelayHandler.prototype.handle = async function (...args) {
        try {
          await originalHandle.apply(this, args);
        } finally {
          finishSecondRelay();
        }
      };
      const secondDelivery = client.waitFor('wire.table-view').timeout(5000).submit();
      await actorClient.sendToActor('player', new TableViewPayload(43)).submit();
      await secondRelay;
      const secondMessage = await secondDelivery;
      assert.equal(secondMessage.name, 'wire.table-view');
      assert.deepEqual(secondMessage.payload, { value: 43 });
      assert.equal(relayResolverCalls, 1);
      assert.equal(TableViewPayload.constructorCalls, 2);
    } finally {
      await client?.close();
      await app?.close();
    }
  }
);
