const assert = require('node:assert/strict');
const test = require('node:test');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');
const { Inject, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const framework = require('../../packages/framework/dist');
const integration = require('../../packages/framework/dist/internal');
const nestjs = require('../../packages/nestjs/dist');
const connector = require('../../packages/stream-connector/dist');

test('Nest fresh Spot creation preserves constructor failure', async () => {
  const failure = new Error('fresh Spot constructor failed');
  let instances = 0;
  class FailingSpot {
    constructor() {
      if (++instances > 1) throw failure;
    }
  }
  const options = nestjs
    .zlinkFramework()
    .addLocationStore(new integration.ZLinkInMemoryProviderLocationStore());
  const mesh = options
    .addRouteMesh('nest-audit-fresh')
    .listen('tcp://127.0.0.1:*')
    .routingId('nest-audit-fresh-node');
  mesh
    .objects()
    .server()
    .addSpotFactory(FailingSpot.name, FailingSpot, (factory) => factory.disableRelocation());
  mesh.channel('nest-audit-fresh').client();
  class Application {}
  Module({ imports: [nestjs.ZLinkModule.forRoot(options.build())], providers: [FailingSpot] })(
    Application
  );
  const app = await NestFactory.createApplicationContext(Application, {
    logger: false,
    abortOnError: false
  });
  try {
    await assert.rejects(
      app
        .get(nestjs.ZLINK_SPOT_MANAGER)
        .create(FailingSpot.name)
        .inMesh('nest-audit-fresh')
        .submit(),
      (error) => error === failure || error.cause === failure
    );
  } finally {
    await app.close();
  }
});

test('Nest fresh Spot creation uses its owning module private dependencies', async () => {
  class PrivateDependency {}
  const created = [];
  class OwnedSpot {
    constructor(dependency) {
      created.push({ instance: this, dependency });
    }
  }
  Inject(PrivateDependency)(OwnedSpot, undefined, 0);
  class OwnerModule {}
  Module({ providers: [PrivateDependency, OwnedSpot] })(OwnerModule);
  const options = nestjs
    .zlinkFramework()
    .addLocationStore(new integration.ZLinkInMemoryProviderLocationStore());
  const mesh = options
    .addRouteMesh('nest-audit-owner')
    .listen('tcp://127.0.0.1:*')
    .routingId('nest-audit-owner-node');
  mesh
    .objects()
    .server()
    .addSpotFactory(OwnedSpot.name, OwnedSpot, (factory) => factory.disableRelocation());
  mesh.channel('nest-audit-owner').client();
  class Application {}
  Module({ imports: [OwnerModule, nestjs.ZLinkModule.forRoot(options.build())] })(Application);
  const app = await NestFactory.createApplicationContext(Application, {
    logger: false,
    abortOnError: false
  });
  try {
    const manager = app.get(nestjs.ZLINK_SPOT_MANAGER);
    await manager.create(OwnedSpot.name).inMesh('nest-audit-owner').submit();
    await manager.create(OwnedSpot.name).inMesh('nest-audit-owner').submit();
    assert.equal(created.length, 3);
    assert.equal(new Set(created.map((entry) => entry.instance)).size, 3);
    assert.ok(created.every((entry) => entry.dependency instanceof PrivateDependency));
    assert.equal(new Set(created.map((entry) => entry.dependency)).size, 1);
  } finally {
    await app.close();
  }
});

test('Nest static stream-only registration discovers session handlers by default', async () => {
  const roleRoot = fs.mkdtempSync(path.join(os.tmpdir(), 'zlink-1083-a3-node-nest-session-'));
  const frameworkPackage = path.resolve(__dirname, '../../packages/framework/dist');
  fs.writeFileSync(
    path.join(roleRoot, 'ping-handler.js'),
    `
const framework = require(${JSON.stringify(frameworkPackage)});
class PingHandler {
  constructor() { this.calls = 0; }
  async handle(context, dispatch) {
    this.calls += 1;
    if (dispatch.canReply) await context.client.reply({ calls: this.calls }).submit();
  }
}
framework.ZLinkPacket('nest.audit.ping')(PingHandler);
module.exports = { PingHandler };
`
  );
  class SessionFactory {
    async create(context) {
      return {
        context,
        async onDispatch(dispatch, message) {
          await context.handlers.tryHandle(dispatch, message);
        }
      };
    }
  }
  class Application {}
  nestjs.zlinkModule(roleRoot, {
    imports: [
      nestjs.ZLinkModule.forRoot(
        nestjs
          .zlinkFramework()
          .addStreamNode('nest-audit-stream')
          .bind('ws://127.0.0.1:*')
          .registerSession(SessionFactory)
          .build()
      )
    ],
    providers: [SessionFactory]
  })(Application);
  const app = await NestFactory.createApplicationContext(Application, {
    logger: false,
    abortOnError: false
  });
  const endpoint = app
    .get(nestjs.ZLINK_FRAMEWORK_RUNTIME)
    .getListenerStatus('stream', 'nest-audit-stream').endpoint;
  const client = connector.zlinkStreamConnectorFactory.create({
    endpoint,
    dispatchMode: connector.ZlinkStreamDispatchMode.Immediate
  });
  try {
    await client.connect();
    class Ping {}
    assert.deepEqual(await client.request(new Ping()).packetName('nest.audit.ping').submit(), {
      calls: 1
    });
    assert.throws(() => app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME), /does not exist|could not find/i);
  } finally {
    await client.close();
    await app.close();
    fs.rmSync(roleRoot, { recursive: true, force: true });
  }
});

test('Nest rejects duplicate Relocation Store registration', () => {
  const store = {};
  const options = nestjs.zlinkFramework();
  assert.equal(options.addRelocationStore(store), options);
  assert.throws(
    () => options.addRelocationStore(store),
    /Relocation Store.*already|Duplicate.*Relocation Store/i
  );
});

test('Framework rejects duplicate Relocation Store registration', () => {
  const store = {};
  assert.throws(
    () =>
      integration.createFrameworkOptions((options) => {
        options.addRelocationStore(store);
        options.addRelocationStore(store);
      }),
    /Relocation Store.*already|Duplicate.*Relocation Store/i
  );
});

test('Nest Actor decorators dispatch selected methods through public Actor client', async () => {
  const calls = [];
  let roomId;
  let joinedRoom;
  class Player {
    constructor(context) {
      this.context = context;
    }
    async onJoinCompleted(completion) {
      if (completion.status === 'accepted' && this.context.spotId === roomId) joinedRoom();
    }
  }
  class PlayerFactory {
    async create(context) {
      return new Player(context);
    }
  }
  class EntrySpot {
    async onCreateActor() {
      return { accepted: true };
    }
  }
  class RoomSpot {
    async onCreate() {
      return { accepted: true };
    }
    async onActorJoin() {
      return { accepted: true };
    }
    async onJoinedActor() {}
    async onLeaveActor() {}
  }
  class Dependency {
    constructor() {
      this.marker = 'injected';
    }
  }
  class Handler {
    constructor(dependency) {
      this.dependency = dependency;
    }
    async handle() {
      throw new Error('default handle must not replace selected method');
    }
    async selectedSend() {
      calls.push(this.dependency.marker);
    }
    async selectedRequest() {
      return { marker: this.dependency.marker, count: calls.length };
    }
    async secondRequest(_spot, actor) {
      actor.context.joinSpot(roomId).defer();
      return { marker: 'second' };
    }
    async dispose() {
      calls.push('disposed');
    }
  }
  Inject(Dependency)(Handler, undefined, 0);
  nestjs.zlinkEntrySpotActorSendHandler({
    actor: Player,
    entrySpot: EntrySpot,
    packetName: 'nest.audit.notice',
    methodName: 'selectedSend'
  })(Handler);
  nestjs.zlinkEntrySpotActorRequestHandler({
    actor: Player,
    entrySpot: EntrySpot,
    packetName: 'nest.audit.request',
    methodName: 'selectedRequest'
  })(Handler);
  nestjs.zlinkEntrySpotActorRequestHandler({
    actor: Player,
    entrySpot: EntrySpot,
    packetName: 'nest.audit.second',
    methodName: 'secondRequest'
  })(Handler);
  class RoomHandler {
    constructor(dependency) {
      this.dependency = dependency;
    }
    async selectedSend() {
      calls.push('room-send');
    }
    async selectedRequest() {
      return { marker: this.dependency.marker, room: true };
    }
  }
  Inject(Dependency)(RoomHandler, undefined, 0);
  nestjs.zlinkSpotActorSendHandler({
    actor: Player,
    spot: RoomSpot,
    packetName: 'nest.audit.room.notice',
    methodName: 'selectedSend'
  })(RoomHandler);
  nestjs.zlinkSpotActorRequestHandler({
    actor: Player,
    spot: RoomSpot,
    packetName: 'nest.audit.room.request',
    methodName: 'selectedRequest'
  })(RoomHandler);
  class Notice {}
  class Request {}
  class SecondRequest {}
  class RoomNotice {}
  class RoomRequest {}
  framework.ZLinkPacket('nest.audit.notice')(Notice);
  framework.ZLinkPacket('nest.audit.request')(Request);
  framework.ZLinkPacket('nest.audit.second')(SecondRequest);
  framework.ZLinkPacket('nest.audit.room.notice')(RoomNotice);
  framework.ZLinkPacket('nest.audit.room.request')(RoomRequest);
  const options = nestjs
    .zlinkFramework()
    .addLocationStore(new integration.ZLinkInMemoryProviderLocationStore());
  const mesh = options
    .addRouteMesh('nest-audit-method')
    .listen('tcp://127.0.0.1:*')
    .routingId('nest-audit-method-node');
  mesh
    .objects()
    .server()
    .addEntrySpot(EntrySpot)
    .addSpotFactory(RoomSpot.name, RoomSpot, (factory) => factory.disableRelocation())
    .addActorFactory('player', PlayerFactory, (factory) => factory.disableRelocation());
  mesh.channel('nest-audit-method').client();
  class Application {}
  Module({
    imports: [nestjs.ZLinkModule.forRoot(options.build())],
    providers: [PlayerFactory, Handler, RoomHandler, Dependency]
  })(Application);
  const app = await NestFactory.createApplicationContext(Application, {
    logger: false,
    abortOnError: false
  });
  try {
    const room = await app
      .get(nestjs.ZLINK_SPOT_MANAGER)
      .create(RoomSpot.name)
      .inMesh('nest-audit-method')
      .submit();
    roomId = room.spot.spotId;
    const created = await app
      .get(nestjs.ZLINK_ACTOR_MANAGER)
      .create('nest-audit-player', 'player')
      .inMesh('nest-audit-method')
      .submit();
    assert.equal(created.status, 'created');
    const client = app.get(nestjs.ZLINK_ACTOR_CLIENT);
    await client.sendToActor('nest-audit-player', new Notice()).submit();
    assert.deepEqual(await client.requestToActor('nest-audit-player', new Request()).submit(), {
      marker: 'injected',
      count: 1
    });
    const moved = new Promise((resolve) => {
      joinedRoom = resolve;
    });
    assert.deepEqual(
      await client.requestToActor('nest-audit-player', new SecondRequest()).submit(),
      { marker: 'second' }
    );
    await moved;
    await client.sendToActor('nest-audit-player', new RoomNotice()).submit();
    assert.deepEqual(await client.requestToActor('nest-audit-player', new RoomRequest()).submit(), {
      marker: 'injected',
      room: true
    });
    assert.equal(calls.includes('room-send'), true);
    assert.equal(await app.get(nestjs.ZLINK_ACTOR_MANAGER).destroy(created.actor), true);
  } finally {
    await app.close();
  }
  assert.equal(calls.filter((value) => value === 'disposed').length, 1);
});

test('Framework Actor registry preserves selected method with explicit packet override', async () => {
  class Handler {
    async selected(_spot, _actor, _context, message) {
      return message.decode();
    }
  }
  framework.ZLinkSpotActorRequest('declared')(
    Handler.prototype,
    'selected',
    Object.getOwnPropertyDescriptor(Handler.prototype, 'selected')
  );
  const registry = new integration.ZLinkSpotActorHandlerRegistryRuntime();
  registry.addHandler(Handler, 'overridden');
  const dispatcher = new integration.ZLinkSpotActorDispatcher({ registry, spot: {} });
  assert.equal(
    await dispatcher.dispatchRequest(
      { context: { actorId: 'override-player' } },
      'overridden',
      framework.ZLinkMessage.from('selected')
    ),
    'selected'
  );
});
