'use strict';
// The process lifecycle and native MeshNode setup follow the repository's
// test/contract/fixtures/user-spot-native-process.js harness.
const flow = require('../helpers/telemetry-log-capture');
const { testRedisUrl } = require('../helpers/test-redis');
const { inspectRuntime } = require('../../../packages/framework/dist/runtime/internal-observation');
const {
  internalFrameworkErrorKind
} = require('../../../packages/framework/dist/runtime/framework-errors-internal');
const { Module, Injectable } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const f = require('@zlink-systems/framework');
const {
  ServiceRelocationObjectRestoreOwner
} = require('../../../packages/framework/dist/internal');
const n = require('@zlink-systems/nestjs');
const {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} = require('@zlink-systems/framework-locations-redis');
const role = process.env.ZLINK_TEST_ROLE;
const meshName = process.env.ZLINK_TEST_MESH;
const routingId = process.env.ZLINK_TEST_ROUTING_ID;

function send(message) {
  if (process.connected)
    process.send(
      JSON.parse(JSON.stringify(message, (_, x) => (typeof x === 'bigint' ? String(x) : x)))
    );
}
class Move {
  constructor(target) {
    this.target = target;
  }
}
let preparedResolve, resumeResolve, pendingDelivery;
let releasePublication;
const deliveryGate = { async waitBeforeSubmit() {} };
class Notice {
  constructor(value = 'notice') {
    this.value = value;
  }
}
class Ask {
  constructor(value = 'ask') {
    this.value = value;
  }
}
class Player {
  constructor(context) {
    this.context = context;
  }
  async onJoinCompleted(completion) {
    send({
      type: 'joined',
      status: completion.status,
      kind: completion.kind,
      spot: String(this.context.spotId)
    });
  }
}
class Factory {
  async create(context) {
    return new Player(context);
  }
}
class Adapter {
  async capture() {
    return new Uint8Array();
  }
  async restore() {}
}
class Entry {
  async onCreateActor() {
    return { accepted: true };
  }
  async onActorJoin() {
    return { accepted: true };
  }
  async onJoinedActor() {}
  async onLeaveActor() {}
}
class Room {
  async onCreate() {
    return { accepted: true };
  }
  async onActorJoin() {
    return { accepted: true };
  }
  async onJoinedActor() {}
  async onLeaveActor() {}
  async onClosing() {
    send({ type: 'closed', spot: String(this.context.spotId) });
  }
}
class MoveHandler {
  async handle(_spot, actor, _context, request) {
    if (request.target) actor.context.joinSpot(request.target).defer();
    else actor.context.joinEntrySpot().defer();
    return { accepted: true };
  }
}
class NoticeHandler {
  async handle(spot, _actor, _context, message) {
    send({ type: 'notice', value: message.value, spot: String(spot.context.spotId) });
  }
}
class AskHandler {
  async handle(spot, actor, _context, message) {
    if (message.value === 'incarnation')
      return { generation: String(actor.context.objectGeneration), node: routingId };
    if (message.value === 'missing')
      throw new f.ZLinkFrameworkException(f.ZLinkFrameworkErrorKind.NotFound, 'application result');
    return { value: message.value, spot: String(spot.context.spotId), node: routingId };
  }
}
for (const Type of [Factory, Adapter, Entry, Room, MoveHandler, NoticeHandler, AskHandler])
  Injectable()(Type);
for (const [decorator, spot] of [
  [n.zlinkEntrySpotActorRequestHandler, 'entrySpot'],
  [n.zlinkSpotActorRequestHandler, 'spot']
]) {
  const Type = spot === 'entrySpot' ? Entry : Room;
  decorator({ actor: () => Player, [spot]: () => Type, packetName: 'Move' })(MoveHandler);
  decorator({ actor: () => Player, [spot]: () => Type, packetName: 'Ask' })(AskHandler);
}
n.zlinkEntrySpotActorSendHandler({
  actor: () => Player,
  entrySpot: () => Entry,
  packetName: 'Notice'
})(NoticeHandler);
n.zlinkSpotActorSendHandler({ actor: () => Player, spot: () => Room, packetName: 'Notice' })(
  NoticeHandler
);
let app, spots, actors, actorClient, routeMesh, current;
async function start() {
  const b = n.zlinkFramework();
  b.configureDispatch().messageFlow('detailed');
  b.configureLocations()
    .pollingIntervalMs(20)
    .ownerLeaseRenewIntervalMs(100)
    .ownerLeaseTtlMs(5000)
    .ownerLeaseFencingMarginMs(500)
    .ownerLeaseRenewTimeoutMs(500);
  b.addLocationStore(
    new ZLinkRedisLocationStore({
      url: testRedisUrl,
      keyPrefix: process.env.ZLINK_TEST_REDIS_PREFIX
    })
  );
  b.addRelocationStore(
    new ZLinkRedisRelocationStore({
      url: testRedisUrl,
      keyPrefix: process.env.ZLINK_TEST_REDIS_PREFIX + 'blobs:'
    })
  );
  const mesh = b
    .addRouteMesh(meshName)
    .listen(process.env.ZLINK_TEST_ENDPOINT)
    .routingId(routingId);
  if (role === 'target')
    mesh
      .objects()
      .server()
      .addEntrySpot(Entry, 'player')
      .addActorFactory('player', Factory, (x) => x.preserveStateWith(Adapter))
      .addSpotFactory('room', Room, (x) => x.disableRelocation());
  else {
    mesh.objects().client();
  }
  if (process.env.ZLINK_TEST_TARGET_ENDPOINT)
    mesh
      .peerConnections()
      .connect(process.env.ZLINK_TEST_TARGET_ROUTING_ID, process.env.ZLINK_TEST_TARGET_ENDPOINT);
  class App {}
  Module({
    imports: [n.ZLinkModule.forRoot(b.build())],
    providers: [
      ...(role === 'target'
        ? [Factory, Adapter, Entry, Room, MoveHandler, NoticeHandler, AskHandler]
        : []),
      {
        provide: Symbol.for('zlink.internal.actor.transport-delivery-gate'),
        useValue: deliveryGate
      }
    ]
  })(App);
  app = await NestFactory.createApplicationContext(App, { logger: false, abortOnError: false });
  const delivery =
    require('../../../packages/framework/dist/runtime/diagnostics/message-flow-delivery').messageFlowDelivery();
  await delivery.ready;
  spots = app.get(n.ZLINK_SPOT_MANAGER);
  actors = app.get(n.ZLINK_ACTOR_MANAGER);
  actorClient = app.get(n.ZLINK_ACTOR_CLIENT);
  routeMesh = app.get(n.ZLINK_ROUTE_MESH_RUNTIME);
  const query = app.get(n.ZLINK_LOCATION_RUNTIME_QUERY);
  const deadline = Date.now() + 10000;
  let descriptor;
  while (Date.now() < deadline) {
    descriptor = (await query.listMeshNodeDescriptors(meshName)).items.find(
      (x) => String(x.rid) === routingId && x.state === 1
    );
    if (descriptor) break;
    await new Promise((r) => setTimeout(r, 20));
  }
  if (!descriptor) throw new Error('node not published Serving');
  process.on('message', onMessage);
  process.once('disconnect', () => void stop());
  send({ type: 'ready', role, pid: process.pid, endpoint: descriptor.endpoint });
}
async function execute(action) {
  const id = process.env.ZLINK_TEST_SPOT;
  switch (action) {
    case 'holdPublication': {
      const openAdmission = ServiceRelocationObjectRestoreOwner.prototype.openAdmission;
      const publication = new Promise((resolve) => {
        releasePublication = () => {
          ServiceRelocationObjectRestoreOwner.prototype.openAdmission = openAdmission;
          resolve();
        };
      });
      // Every committed target reaches admission, even when publication cleanup fails.
      ServiceRelocationObjectRestoreOwner.prototype.openAdmission = async function (...args) {
        send({ type: 'publication-held' });
        await publication;
        return await openAdmission.apply(this, args);
      };
      return true;
    }
    case 'resumePublication':
      releasePublication();
      return true;
    case 'waitNewOwner':
    case 'waitRouteReady':
      for await (const x of routeMesh.observe(meshName, 64, AbortSignal.timeout(10000))) {
        if (
          x.status.isReady &&
          x.status.peers.some(
            (p) =>
              String(p.nodeRid) ===
                (action === 'waitNewOwner'
                  ? process.env.ZLINK_TEST_NEXT_ROUTING_ID
                  : process.env.ZLINK_TEST_TARGET_ROUTING_ID) && p.state === f.ZLinkPeerState.Ready
          )
        )
          return true;
      }
      throw new Error('peer not Ready');
    case 'create':
    case 'getOrCreate': {
      const result = await spots.getOrCreate(id, 'room').inMesh(meshName).submit();
      current = result.spot;
      return { state: result.state, spot: result.spot };
    }
    case 'close':
      return await spots.close(current);
    case 'findActor':
      return await actors.find('alice');
    case 'destroyActor':
      return await actors.destroy(await actors.find('alice'));
    case 'relocate':
      return await app
        .get(n.ZLINK_FRAMEWORK_RUNTIME)
        .relocate({ mode: f.ZLinkFrameworkRelocationMode.PlannedMaintenance, deadlineMs: 5000 });
    case 'actorRouteCached':
      return (
        app
          .get(n.ZLINK_FRAMEWORK_RUNTIME)
          [inspectRuntime]()
          .actorRouteCache?.directActorRouteKeys.includes('alice') ?? false
      );
    case 'applicationNotFound':
      return await actorClient.requestToActor('alice', new Ask('missing')).submit();
    case 'createActor':
      return await actors.getOrCreate('alice', 'player').inMesh(meshName).submit();
    case 'prepareAsk':
    case 'prepareNotice': {
      const prepared = new Promise((resolve) => {
        preparedResolve = resolve;
      });
      const resume = new Promise((resolve) => {
        resumeResolve = resolve;
      });
      deliveryGate.waitBeforeSubmit = async () => {
        preparedResolve();
        await resume;
      };
      pendingDelivery =
        action === 'prepareAsk'
          ? actorClient.requestToActor('alice', new Ask()).submit()
          : actorClient.sendToActor('alice', new Notice()).submit();
      await prepared;
      return true;
    }
    case 'resumeDelivery': {
      deliveryGate.waitBeforeSubmit = async () => {};
      resumeResolve();
      return await pendingDelivery;
    }
    case 'join':
      return await actorClient.requestToActor('alice', new Move(id)).submit();
    case 'entry':
      return await actorClient.requestToActor('alice', new Move(null)).submit();
    case 'ask':
      return await actorClient.requestToActor('alice', new Ask()).submit();
    case 'askGeneration':
      return await actorClient.requestToActor('alice', new Ask('incarnation')).submit();
    case 'notice':
      await actorClient.sendToActor('alice', new Notice()).submit();
      return true;
    default:
      throw new Error(action);
  }
}
async function onMessage(message) {
  if (message.type === 'stop') return await stop();
  if (typeof message.id !== 'number') return;
  try {
    send({ type: 'result', id: message.id, value: await execute(message.action) });
  } catch (e) {
    send({
      type: 'error',
      id: message.id,
      message: JSON.stringify({
        kind: e.kind,
        internalKind:
          e instanceof f.ZLinkFrameworkException ? internalFrameworkErrorKind(e) : undefined,
        message: e.message,
        stack: e.stack,
        cause: e.cause?.stack
      })
    });
  }
}
let stopping = false;
async function stop() {
  if (stopping) return;
  stopping = true;
  await app?.close();
  await flow.close();
  process.exit(0);
}
start().catch((e) => {
  send({ type: 'fatal', message: e.stack });
  process.exit(1);
});
