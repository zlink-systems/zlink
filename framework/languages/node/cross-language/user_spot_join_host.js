/* SPDX-License-Identifier: LicenseRef-zlink-License-1.0 */

const fs = require('node:fs');
const path = require('node:path');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const framework = require('../packages/framework/dist');
const nestjs = require('../packages/nestjs/dist');
const locations = require('../packages/framework-locations-redis/dist');
const assert = require('node:assert/strict');
const { randomUUID } = require('node:crypto');
const { OutageStore, until, configureOutage } = require('./outage-control');
const { decodeActorAuthorityPayload } = require('../packages/framework/dist/runtime/actors/actor-authority-payload-codec');
const { decodeActorJoin28 } = require(
  '../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated.js'
);

const CANONICAL_ACTOR_JOIN_PACKET = 'ZLinkFrameworkActorJoinRequest';
// Service-wire command byte of the Actor Join header frame (offset 3, after
// the ZM magic and the wire version).
const ACTOR_JOIN_WIRE_COMMAND = 28;
const RELOCATION_ACTOR_TYPE = 'cross-lang-relocation-actor-type';
const mode = process.argv[2];
const args = parseArgs(process.argv.slice(3));
let userSpotTargetJoined;
let resolveUserSpotTargetJoined;
let outageStore;
let actorCreations = 0;
let probeExecutions = 0;

function locationStore(builder, keyPrefix, target) {
  const store = new locations.ZLinkRedisLocationStore({
    url: `redis://${require_('redis-endpoint')}`,
    keyPrefix: `${keyPrefix}:location`
  });
  if (!args['outage-control-prefix']) return store;
  const profile = configureOutage(builder);
  outageStore = new OutageStore(store, args, appendEvent, target, profile);
  return outageStore;
}

function parseArgs(argv) {
  const values = {};
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument.startsWith('--') && index + 1 < argv.length) {
      values[argument.slice(2)] = argv[index + 1];
      index += 1;
    }
  }
  return values;
}

function require_(name) {
  const value = args[name];
  if (!value) throw new Error(`--${name} is required`);
  return value;
}

function appendEvent(line) {
  console.log(line);
  if (args['event-file']) fs.appendFileSync(args['event-file'], `${line}\n`);
}

async function waitForEvent(file, marker) {
  if (fs.readFileSync(file, 'utf8').includes(marker)) return;
  await new Promise((resolve, reject) => {
    const changed = () => {
      if (!fs.readFileSync(file, 'utf8').includes(marker)) return;
      clearTimeout(deadline);
      fs.unwatchFile(file, changed);
      resolve();
    };
    fs.watchFile(file, { interval: 50 }, changed);
    const deadline = setTimeout(() => {
      fs.unwatchFile(file, changed);
      reject(new Error(`timed out waiting for ${marker}`));
    }, 60_000);
  });
}

function appendFlow(line) {
  if (args['event-file']) fs.appendFileSync(`${args['event-file']}.flow`, `${line}\n`);
}

function writeReady() {
  if (args['ready-file']) fs.writeFileSync(args['ready-file'], 'ready\n');
}

function errorKindName(kind) {
  return framework.ZLinkFrameworkErrorKind[kind] ?? String(kind ?? 'InternalFailure');
}

function installMessageFlowCapture() {
  if (!args['event-file']) return undefined;
  fs.writeFileSync(`${args['event-file']}.flow`, '');
  const provider = new LoggerProvider({
    processors: [{
      onEmit(record) {
        const fields = record.attributes;
        appendFlow(
          `zlink flow: label=node-user-spot-join`
          + ` phase=${fields.phase ?? '<null>'}`
          + ` surface=${fields.surface ?? '<null>'}`
          + ` kind=${fields.message_kind ?? '<null>'}`
          + ` packet=${fields.packet_name ?? '<null>'}`
          + ` flow=${fields.flow_id ?? '<null>'}`
          + ` origin=${fields.flow_origin ?? '<null>'}`
        );
      },
      forceFlush: async () => undefined,
      shutdown: async () => undefined
    }]
  });
  logs.setGlobalLoggerProvider(provider);
  return provider;
}

function installCanonicalWireProbe(app, meshName) {
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  const nativeNode = runtime.spotNodeRuntime?.meshNode(meshName);
  const stateful = nativeNode?.stateful;
  const serviceTransport = stateful?.raw;
  if (serviceTransport === undefined
    || typeof serviceTransport.requestService !== 'function') {
    throw new Error('canonical Actor Join wire probe could not find the active Node service transport');
  }

  // The Actor Join wire command is the only command carried by a command-28
  // header, and every remote Join attempt - the plain sender and the durable
  // lifecycle sender that replays frozen frames alike - hands its frames to
  // this one service transport call. Observing the transport instead of one
  // sender method keeps the probe attached when the runtime changes which
  // sender owns a Join.
  const originalRequestService = serviceTransport.requestService;
  serviceTransport.requestService = function requestServiceWithCanonicalProbe(
    targetNodeRid,
    parts,
    timeoutMs
  ) {
    if (parts[0]?.[3] === ACTOR_JOIN_WIRE_COMMAND) {
      try {
        const decoded = decodeActorJoin28(parts);
        const packetName = decoded.payload?.packetName ?? '<none>';
        const canonical = packetName === CANONICAL_ACTOR_JOIN_PACKET;
        appendFlow(
          `canonical actorJoin: wire_command=28 canonical=${canonical}`
          + ` packet=${packetName}`
          + ` correlation=${decoded.correlation}`
          + ` actor=${decoded.actor.id}`
          + ` actor_node=${Buffer.from(decoded.actor.targetNodeRid).toString('utf8')}`
          + ` actor_node_generation=${decoded.actor.targetNodeGeneration}`
          + ` target=${targetNodeRid}`
          + ` target_node_generation=${decoded.targetSpot.targetNodeGeneration}`
        );
      } catch (error) {
        appendFlow(`canonical actorJoin: wire_command=decode-failed error=${String(error)}`);
      }
    }
    if (parts[0]?.[3] === 22) {
      appendEvent(`stateful-22-wire|target=${targetNodeRid}|bytes=${parts[0].length}`);
    }
    if (parts[0]?.[3] === 25) {
      appendEvent(`stateful-25-wire|target=${targetNodeRid}|bytes=${parts[0].length}`);
    }
    return originalRequestService.call(this, targetNodeRid, parts, timeoutMs);
  };

  if (args['force-private-join'] === '1') {
    stateful.canOriginateCanonicalActorJoin = () => false;
    appendFlow('canonical actorJoin: forced_private=true');
  }

  if (mode !== 'user-spot-join-source-multiattempt') return;
  if (typeof nativeNode.joinActorSpotCanonical !== 'function'
    || typeof stateful.joinActorCanonical !== 'function') {
    throw new Error('canonical multi-attempt ingress could not find the active canonical Actor Join transport');
  }

  const originalJoinActorCanonical = stateful.joinActorCanonical;
  let attempt = 0;
  stateful.joinActorCanonical = function joinActorCanonicalWithTerminalCapture(...callArgs) {
    attempt += 1;
    const attemptName = attempt === 1 ? 'first' : attempt === 2 ? 'second' : `unexpected-${attempt}`;
    const pending = originalJoinActorCanonical.apply(this, callArgs);
    appendEvent(
      `canonical-multiattempt-sent|attempt=${attemptName}|correlation=${pending.id}`
    );
    void pending.promise.then(
      result => appendEvent(
        `canonical-multiattempt-terminal|attempt=${attemptName}|correlation=${pending.id}`
        + `|terminal=${result.terminalResult}|failure=${result.failureCode}`
      ),
      error => appendEvent(
        `canonical-multiattempt-terminal|attempt=${attemptName}|correlation=${pending.id}`
        + `|error=${errorKindName(error?.kind)}|message=${String(error?.message ?? error)}`
      )
    );
    return pending;
  };

  const originalJoinActorSpotCanonical = nativeNode.joinActorSpotCanonical;
  nativeNode.joinActorSpotCanonical = function joinActorSpotCanonicalWithMultiAttempt(...callArgs) {
    const first = originalJoinActorSpotCanonical.apply(this, callArgs);
    // The normal public joinSpot().defer() owns the first attempt.  This
    // test-only ingress sends a second command-28 through the same canonical
    // transport immediately, so its OperationRegistry reservation produces a
    // distinct correlation without a second public deferred join.
    originalJoinActorSpotCanonical.apply(this, callArgs);
    return first;
  };
}

function installCanonicalReceiverProbe(app) {
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  const receiver = runtime.boundSessionRelay?.actorJoins;
  if (receiver === undefined || typeof receiver.prepareCanonicalActorJoin !== 'function') {
    throw new Error('canonical Actor Join receiver probe could not find the active Node service runtime');
  }

  const originalPrepare = receiver.prepareCanonicalActorJoin;
  receiver.prepareCanonicalActorJoin = async function prepareCanonicalActorJoinWithProbe(join) {
    appendFlow(
      `canonical actorJoin: wire_command=28 canonical=true packet=${CANONICAL_ACTOR_JOIN_PACKET}`
      + ` receiver=prepareCanonicalActorJoin ingress=true actor=${join.actorId}`
      + ` actor_node=${join.actorNodeRid} target_authority=${join.expectedAuthorityOwnerGeneration}`
    );
    try {
      return await originalPrepare.call(this, join);
    } catch (error) {
      const kind = errorKindName(error?.kind);
      const message = String(error?.message ?? error);
      appendFlow(
        `canonical actorJoin: receiver=prepareCanonicalActorJoin outcome=error`
        + ` error_kind=${kind} error_message=${message}`
      );
      appendEvent(
        `canonical-actor-join-prepare-failed|kind=${kind}|message=${message}`
      );
      throw error;
    }
  };
}

class UserSpotActorCreateReq {
  constructor(stateVersion) { this.stateVersion = stateVersion; }
}

class UserSpotCreateReq {
  constructor(marker) { this.marker = marker; }
}

class BeginUserSpotJoinReq {
  constructor(targetSpotId, marker) {
    this.targetSpotId = targetSpotId;
    this.marker = marker;
  }
}

class UserSpotJoinReq {
  constructor(marker) { this.marker = marker; }
}

class UserSpotProbeReq {
  constructor(marker) { this.marker = marker; }
}

class UserSpotDiscoveryProbeHandler {
  async handle(request) {
    return { marker: request.marker, nodeRid: require_('node-rid') };
  }
}
Injectable()(UserSpotDiscoveryProbeHandler);

class RelocationActor {
  constructor(actorId, context) {
    this.instanceId = randomUUID();
    this.creationCount = ++actorCreations;
    this.actorId = actorId;
    this.context = context;
    this.stateVersion = 0;
    this.applicationState = new Uint8Array();
  }

  async onJoinCompleted(completion) {
    if (completion.status === 'failed') {
      appendEvent(`user-spot-join-failed|kind=${errorKindName(completion.kind)}`);
      return;
    }
    appendEvent(`user-spot-join-completed|status=${completion.status}`);
  }
}

class RelocationActorFactory {
  async create(context) {
    return new RelocationActor(context.actorId, context);
  }
}
Injectable()(RelocationActorFactory);

class RelocationActorAdapter {
  async capture(actor, signal) {
    signal.throwIfAborted();
    const payload = new Uint8Array(8 + actor.applicationState.byteLength);
    const view = new DataView(payload.buffer);
    view.setInt32(0, actor.stateVersion, true);
    view.setInt32(4, actor.applicationState.byteLength, true);
    payload.set(actor.applicationState, 8);
    return payload;
  }

  async restore(actor, payload, signal) {
    signal.throwIfAborted();
    if (payload.byteLength < 8) throw new Error('relocation binary payload header is truncated');
    const view = new DataView(payload.buffer, payload.byteOffset, payload.byteLength);
    const applicationStateBytes = view.getInt32(4, true);
    if (applicationStateBytes < 0 || payload.byteLength !== 8 + applicationStateBytes) {
      throw new Error('relocation binary application state size is invalid');
    }
    actor.stateVersion = view.getInt32(0, true);
    actor.applicationState = payload.slice(8);
  }
}
Injectable()(RelocationActorAdapter);

class RelocationEntrySpot {
  async onCreateActor(actor, request) {
    const create = request.toEncodedPayload().isEmpty() ? {} : request.decode();
    actor.stateVersion = create.stateVersion ?? 1;
    actor.applicationState = Uint8Array.from([1, 2, 3, 4]);
    return { accepted: true };
  }

  async onJoinedActor() {}
  async onLeaveActor() {}
  async onDisconnectActor() {}
}
Injectable()(RelocationEntrySpot);

class RelocationUserSpot {
  async onCreate() {
    return { accepted: true };
  }

  async onActorJoin(actorId, _request) {
    appendEvent(`user-spot-admission|accepted=true|actor=${actorId}|spot=${this.context.spotId}`);
    return { accepted: true };
  }

  async onJoinedActor(actor) {
    appendEvent(
      `user-spot-joined|actor=${actor.actorId}|spot=${this.context.spotId}`
      + `|nodeRid=${this.context.nodeRid}`
    );
    resolveUserSpotTargetJoined?.();
  }

  async onLeaveActor() {}
  async onDisconnectActor() {}
}
Injectable()(RelocationUserSpot);

class BeginUserSpotJoinHandler {
  async handle(_spot, actor, _context, request) {
    actor.context
      .joinSpot(request.targetSpotId, new UserSpotJoinReq(request.marker))
      .timeout(30_000)
      .defer();
    return { accepted: true, actorId: actor.actorId, targetSpotId: request.targetSpotId };
  }
}
Injectable()(BeginUserSpotJoinHandler);
nestjs.zlinkEntrySpotActorRequestHandler({
  actor: () => RelocationActor,
  entrySpot: () => RelocationEntrySpot,
  packetName: 'BeginUserSpotJoinReq'
})(BeginUserSpotJoinHandler);

class SourceProbeHandler {
  async handle(_spot, actor, _context, request) {
    probeExecutions += 1;
    return {
      actorId: actor.actorId,
      spotId: String(actor.context.spotId),
      nodeRid: require_('node-rid'),
      stateVersion: actor.stateVersion,
      marker: request.marker,
      instanceId: actor.instanceId,
      creationCount: actor.creationCount
    };
  }
}
Injectable()(SourceProbeHandler);
nestjs.zlinkEntrySpotActorRequestHandler({
  actor: () => RelocationActor,
  entrySpot: () => RelocationEntrySpot,
  packetName: 'UserSpotProbeReq'
})(SourceProbeHandler);

class TargetProbeHandler {
  async handle(_spot, actor, _context, request) {
    probeExecutions += 1;
    const nodeRid = require_('node-rid');
    appendEvent(`user-spot-probe-served|nodeRid=${nodeRid}|actor=${actor.actorId}`);
    return {
      actorId: actor.actorId,
      spotId: String(actor.context.spotId),
      nodeRid,
      stateVersion: actor.stateVersion,
      marker: request.marker
    };
  }
}
nestjs.zlinkSpotActorRequestHandler({
  actor: () => RelocationActor,
  spot: () => RelocationUserSpot,
  packetName: 'UserSpotProbeReq'
})(TargetProbeHandler);

async function userSpotJoinSource() {
  const meshName = require_('mesh-name');
  const actorId = require_('actor-id');
  const targetSpotId = require_('spot-id');
  const keyPrefix = args['redis-key-prefix'] ?? 'zlink-cross-user-spot-join';
  const flowProvider = installMessageFlowCapture();

  class UserSpotJoinSourceModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.configureDispatch().messageFlow('normal');
        builder.addLocationStore(locationStore(builder, keyPrefix, false));
        builder.addRelocationStore(new locations.ZLinkRedisRelocationStore({
          url: `redis://${require_('redis-endpoint')}`,
          keyPrefix: `${keyPrefix}:relocation`
        }));
        const mesh = builder.addRouteMesh(meshName)
          .listen(require_('bind-endpoint'))
          .routingId(require_('node-rid'))
          .setPlacementWeight(100);
        mesh.addRequestHandler(
          'UserSpotDiscoveryProbeReq',
          UserSpotDiscoveryProbeHandler
        );
        mesh.channel(meshName).server();
        const objects = mesh.objects().server();
        objects.addEntrySpot(RelocationEntrySpot, RELOCATION_ACTOR_TYPE);
        objects.addActorFactory(
          RELOCATION_ACTOR_TYPE,
          RelocationActorFactory,
          factory => factory.preserveStateWith(RelocationActorAdapter)
        );
        return builder.build();
      }
    })],
    providers: [
      RelocationActorFactory,
      RelocationActorAdapter,
      RelocationEntrySpot,
      BeginUserSpotJoinHandler,
      SourceProbeHandler,
      UserSpotDiscoveryProbeHandler
    ]
  })(UserSpotJoinSourceModule);

  const app = await NestFactory.createApplicationContext(
    UserSpotJoinSourceModule,
    { logger: false, abortOnError: false }
  );
  installCanonicalWireProbe(app, meshName);
  writeReady();

  const routeMesh = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
  const discoveryDeadline = Date.now() + 60_000;
  while (routeMesh.snapshot(meshName).readyPeerCount === 0) {
    if (Date.now() >= discoveryDeadline) {
      throw new Error('automatic RouteMesh discovery did not admit the .NET target');
    }
    await new Promise(resolve => setTimeout(resolve, 100));
  }
  appendEvent('user-spot-source-peer-ready|ready=true');
  const startFile = require_('start-file');
  const startDeadline = Date.now() + 60_000;
  while (!fs.existsSync(startFile)) {
    if (Date.now() >= startDeadline) {
      throw new Error('timed out waiting for the reciprocal discovery barrier');
    }
    await new Promise(resolve => setTimeout(resolve, 25));
  }

  const actorManager = app.get(nestjs.ZLINK_ACTOR_MANAGER, { strict: false });
  const created = await actorManager
    .getOrCreate(actorId, RELOCATION_ACTOR_TYPE)
    .inMesh(meshName)
    .request(new UserSpotActorCreateReq(7))
    .timeout(15_000)
    .submit();
  appendEvent(`user-spot-source-actor-created|status=${created.status}|node=${created.actor?.nodeRid}`);

  const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT, { strict: false });
  if (outageStore) {
    await runOutageSource(app, actorManager, actorClient, actorId, targetSpotId);
    await new Promise(() => {});
  }
  const reply = await actorClient
    .requestToActor(actorId, new BeginUserSpotJoinReq(targetSpotId, 'canonical-28'))
    .timeout(45_000)
    .submit();
  appendEvent(
    `user-spot-join-request-reply|accepted=${reply.accepted === true}`
    + `|actor=${reply.actorId}|spot=${reply.targetSpotId}`
  );

  if (process.env.ZLINK_NODE_JAVA_STATEFUL_PROBE === '1') {
    const javaEvents = path.join(path.dirname(require_('event-file')), 'java-user-spot-target.events');
    await waitForEvent(javaEvents, 'user-spot-probe|nodeRid=java-user-spot-join-target');
    const spots = app.get(nestjs.ZLINK_SPOT_MANAGER, { strict: false });
    const targetSpot = await spots.find(targetSpotId);
    if (targetSpot?.nodeRid !== 'java-user-spot-join-target') {
      throw new Error(`Spot lookup reached ${targetSpot?.nodeRid}`);
    }
    const spotOutbound = app.get(nestjs.ZLINK_SPOT_OUTBOUND, { strict: false });
    try {
      await spotOutbound.requestToSpot(targetSpotId, new UserSpotProbeReq('node-java-wire-22'))
        .timeout(5_000).submit();
      throw new Error('Spot request unexpectedly found a handler');
    } catch (error) {
      if (error.message === 'Spot request unexpectedly found a handler') throw error;
      if (!String(error.message).includes("HANDLER_MISSING for packet 'UserSpotProbeReq'")) throw error;
      appendEvent(`stateful-22|kind=${errorKindName(error.kind)}|target=${targetSpot.nodeRid}`);
    }

    // The ordinary Actor client captures handoff and relays remote requests through Spot (22).
    // This test selects the same backend's direct Actor request path to verify command 25.
    const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
    const resolution = await runtime.createActorLocationResolver().resolveDirectActorRoute(actorId);
    if (resolution.kind !== 'ready') throw new Error(`Actor route is ${resolution.kind}`);
    const actorRoute = resolution.route;
    if (actorRoute.actorRef.nodeRid !== 'java-user-spot-join-target') {
      throw new Error(`Actor route still points to ${actorRoute.actorRef.nodeRid}`);
    }
    const backend = runtime.spotNodeRuntime.meshNode(meshName);
    backend.stateful.rememberActorRoute({
      actor: {
        nodeRid: actorRoute.actorRef.nodeRid,
        actorId: actorRoute.actorRef.actorId,
        generation: actorRoute.actorRef.objectGeneration
      },
      targetNodeGeneration: actorRoute.ownerNodeGeneration,
      authorityOwnerGeneration: actorRoute.authorityOwnerGeneration,
      ownerLeaseGeneration: actorRoute.ownerLeaseGeneration
    });
    const directActorClient = new actorClient.constructor({
      ...actorClient.options,
      routeTransport: undefined,
      handoffCapture: undefined,
      staleActorRefPredicate: undefined,
      locationResolver: () => ({
        resolveDirectActorRoute: async () => resolution,
        invalidateActorRoute() {}
      })
    });
    const actorProbe = await directActorClient
      .requestToActor(actorId, new UserSpotProbeReq('node-java-wire-25'))
      .timeout(5_000)
      .submit();
    if (actorProbe.nodeRid !== 'java-user-spot-join-target') {
      throw new Error(`Actor request reached ${actorProbe.nodeRid}`);
    }
    appendEvent(`stateful-25|nodeRid=${actorProbe.nodeRid}|actor=${actorProbe.actorId}`);
  }

  await new Promise(() => {});
  await app.close();
  await flowProvider?.shutdown();
}

async function userSpotJoinTarget() {
  const meshName = require_('mesh-name');
  const spotId = require_('spot-id');
  const keyPrefix = args['redis-key-prefix'] ?? 'zlink-cross-user-spot-join';
  const flowProvider = installMessageFlowCapture();
  userSpotTargetJoined = new Promise(resolve => {
    resolveUserSpotTargetJoined = resolve;
  });

  class UserSpotJoinTargetModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.configureDispatch().messageFlow('normal');
        builder.addLocationStore(locationStore(builder, keyPrefix, true));
        builder.addRelocationStore(new locations.ZLinkRedisRelocationStore({
          url: `redis://${require_('redis-endpoint')}`,
          keyPrefix: `${keyPrefix}:relocation`
        }));
        const mesh = builder.addRouteMesh(meshName)
          .listen(require_('bind-endpoint'))
          .routingId(require_('node-rid'))
          .setPlacementWeight(100);
        mesh.channel(meshName).server();
        const objects = mesh.objects().server();
        objects.addEntrySpot(RelocationEntrySpot, RELOCATION_ACTOR_TYPE);
        objects.addSpotFactory(
          'cross-lang-relocation-user-spot-type',
          RelocationUserSpot,
          factory => factory.disableRelocation()
        );
        objects.addActorFactory(
          RELOCATION_ACTOR_TYPE,
          RelocationActorFactory,
          factory => factory.preserveStateWith(RelocationActorAdapter)
        );
        return builder.build();
      }
    })],
    providers: [
      RelocationActorFactory,
      RelocationActorAdapter,
      RelocationEntrySpot,
      RelocationUserSpot,
      BeginUserSpotJoinHandler,
      TargetProbeHandler
    ]
  })(UserSpotJoinTargetModule);

  const app = await NestFactory.createApplicationContext(
    UserSpotJoinTargetModule,
    { logger: false, abortOnError: false }
  );
  installCanonicalReceiverProbe(app);
  const spots = app.get(nestjs.ZLINK_SPOT_MANAGER, { strict: false });
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  let serving = false;
  for await (const observed of runtime.observe()) {
    if (observed.status.state === framework.ZLinkFrameworkRuntimeState.Serving) {
      serving = true;
      break;
    }
    if (observed.status.state === framework.ZLinkFrameworkRuntimeState.Error
      || observed.status.state === framework.ZLinkFrameworkRuntimeState.Stopped) {
      throw new Error(`Framework runtime reached ${observed.status.state} before serving`);
    }
  }
  if (!serving) throw new Error('Framework runtime observation ended before serving');
  const created = await spots
    .getOrCreate(spotId, 'cross-lang-relocation-user-spot-type')
    .inMesh(meshName)
    .request(new UserSpotCreateReq('cross-language-user-spot'))
    .timeout(15_000)
    .submit();
  appendEvent(
    `user-spot-created|spot=${created.spot.spotId}|nodeRid=${created.spot.nodeRid}|state=${created.state}`
  );
  // Match the reciprocal .NET target: after its fixed User Spot is created,
  // it must not be eligible for the source's Entry-Spot actor placement.
  // Keep the actor factory so this target can materialize the actor after the
  // User-Spot join transfers ownership. A cell that wants the create itself to
  // travel here passes --placement-weight 100 and gives the source 0 instead.
  const runtimeOptions = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false });
  runtimeOptions.mesh(meshName).placementWeight =
    Number.parseInt(args['placement-weight'] ?? '0', 10);
  writeReady();

  const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT, { strict: false });
  if (outageStore) {
    await runOutageTarget(app, actorClient, created.spot.nodeRid);
    await new Promise(() => {});
  }
  void probeUserSpotTarget(actorClient, created.spot.nodeRid);

  await new Promise(() => {});
  await app.close();
  await flowProvider?.shutdown();
}

async function probeUserSpotTarget(actorClient, targetNodeRid) {
  const joined = await Promise.race([
    userSpotTargetJoined.then(() => true),
    new Promise(resolve => setTimeout(() => resolve(false), 75_000))
  ]);
  if (!joined) {
    appendEvent(`user-spot-probe-timeout|targetRid=${targetNodeRid}|failure=join-not-observed`);
    return;
  }

  const deadline = Date.now() + 90_000;
  let lastFailure = 'none';
  while (Date.now() < deadline) {
    try {
      const reply = await actorClient
        .requestToActor(require_('actor-id'), new UserSpotProbeReq('target-owner-probe'))
        .timeout(5_000)
        .submit();
      if (reply.nodeRid === targetNodeRid) {
        appendEvent(
          `user-spot-probe|nodeRid=${reply.nodeRid}|targetRid=${targetNodeRid}`
          + `|actor=${reply.actorId}|stateVersion=${reply.stateVersion}`
        );
        return;
      }
      lastFailure = `unexpected-owner:${reply.nodeRid}`;
    } catch (error) {
      lastFailure = `${errorKindName(error?.kind)}:${String(error?.message ?? error)}`;
    }
    await new Promise(resolve => setTimeout(resolve, 500));
  }
  appendEvent(`user-spot-probe-timeout|targetRid=${targetNodeRid}|failure=${lastFailure}`);
}

async function runOutageSource(app, actorManager, actorClient, actorId, targetSpotId) {
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  const probe = marker => actorClient.requestToActor(actorId, new UserSpotProbeReq(marker))
    .timeout(5_000).submit();
  let initial;
  let join;
  await outageStore.commands(async phase => {
    switch (phase) {
      case 'steady': {
        initial = await probe('outage-steady');
        outageStore.owner = runtime.locationOwner.currentRuntime.currentOwnerToken;
        outageStore.baseline = await outageStore.observer.readAuthority(outageStore.authorityKey);
        assert.equal(outageStore.baseline.kind, 'snapshot');
        assert.equal(decodeActorAuthorityPayload(outageStore.baseline.payload)?.state, 'ready');
        assert.equal(outageStore.baseline.ownerLeaseGeneration, outageStore.owner.leaseGeneration);
        appendEvent(`outage-identity|instance=${initial.instanceId}|creations=${initial.creationCount}`
          + `|object=${outageStore.baseline.objectGeneration}|authority=${outageStore.baseline.authorityOwnerGeneration}`
          + `|lease=${outageStore.owner.leaseGeneration}`);
        break;
      }
      case 'outage':
        outageStore.beginOutage(runtime.locationOwner.currentRuntime);
        await until(() => outageStore.failed, 'actual Store failure');
        await outageStore.captureOutage();
        break;
      case 'expired':
        await until(async () => (await outageStore.observer.readOwnerLease(outageStore.owner.ownerId)).kind === 'missing', 'old lease expiry');
        break;
      case 'recover':
        outageStore.recovering = true;
        outageStore.outage = false;
        await until(() => outageStore.newOwner, 'new lease claim');
        appendEvent(`outage-new-lease|old=${outageStore.owner.leaseGeneration}|new=${outageStore.newOwner.leaseGeneration}`);
        break;
      case 'release-descriptor':
        await outageStore.release('descriptor');
        break;
      case 'release-cas':
        await outageStore.release('cas');
        break;
      case 'partial': {
        const pending = args['outage-order'] === 'descriptor-first' ? 'cas' : 'descriptor';
        await until(() => outageStore.holds.has(pending), `${pending} remains held`);
        const before = probeExecutions;
        await assert.rejects(probe('outage-partial'), error =>
          error.kind === framework.ZLinkFrameworkErrorKind.ShuttingDown);
        assert.equal(probeExecutions, before);
        appendEvent('outage-partial|kind=ShuttingDown|handlers=0');
        break;
      }
      case 'resumed': {
        assert(outageStore.preserved, 'Preserve confirmation missing');
        assert(outageStore.newOwner, 'new owner lease missing');
        assert.notEqual(outageStore.newOwner.leaseGeneration, outageStore.owner.leaseGeneration);
        assert(Number.isFinite(outageStore.descriptorConfirmed), 'descriptor confirmation missing');
        assert(Number.isFinite(outageStore.casConfirmed), 'CAS confirmation missing');
        await until(() => runtime.locationOwner.currentRuntime.boundExecutionUsable, 'source execution gate resumed');
        const reply = await probe('outage-resumed');
        assert.equal(reply.instanceId, initial.instanceId);
        assert.equal(reply.creationCount, initial.creationCount);
        const fence = runtime.actorHandoff.options.currentOwnerFence(actorId);
        assert.equal(fence.ownerLeaseGeneration, String(outageStore.newOwner.leaseGeneration));
        appendEvent(`outage-resumed|instance=${reply.instanceId}|creations=${reply.creationCount}|lease=${fence.ownerLeaseGeneration}`);
        outageStore.recovering = false;
        break;
      }
      case 'relocation':
        join = Promise.resolve(actorClient.requestToActor(actorId,
          new BeginUserSpotJoinReq(targetSpotId, 'outage-relocation'))
          .timeout(45_000).submit()).then(
          value => ({ value }), error => ({ error }));
        break;
      case 'completed': {
        const result = await join;
        if (result.error) throw result.error;
        assert.equal(result.value.accepted, true);
        // Recovered source/coordinator fence is observed only on .NET; the other sources record Preparing/Captured in authority after https://github.com/zlink-systems/zlink/issues/4718.
        const authority = await outageStore.observer.readAuthority(outageStore.authorityKey);
        assert.equal(authority.kind, 'snapshot');
        assert.notEqual(authority.ownerId, outageStore.owner.ownerId);
        assert.equal(authority.objectGeneration, outageStore.baseline.objectGeneration);
        break;
      }
      default: throw new Error(`unsupported source outage command ${phase}`);
    }
  });
}

async function runOutageTarget(app, actorClient, targetNodeRid) {
  const runtime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  const actorManager = app.get(nestjs.ZLINK_ACTOR_MANAGER, { strict: false });
  const actorId = require_('actor-id');
  await outageStore.commands(async phase => {
    switch (phase) {
      case 'steady':
        outageStore.owner = runtime.locationOwner.currentRuntime.currentOwnerToken;
        outageStore.armed = true;
        break;
      case 'staged': {
        await until(() => outageStore.holds.has('commit'), 'target NewOwner CAS');
        const found = await actorManager.find(actorId);
        assert.equal(String(found.nodeRid), require_('peer-rid'));
        assert.equal(probeExecutions, 0);
        assert.equal(runtime.actorHandoff.options.currentOwnerFence(actorId), undefined);
        appendEvent('outage-staged|find=source|handlers=0|fence=absent');
        break;
      }
      case 'commit': await outageStore.release('commit'); break;
      case 'completed':
        await until(() => userSpotTargetJoined && runtime.actorManager.publishedState(actorId), 'target publication');
        await userSpotTargetJoined;
        for (const marker of ['outage-target', 'outage-followup']) {
          const reply = await actorClient.requestToActor(actorId, new UserSpotProbeReq(marker))
            .timeout(5_000).submit();
          assert.equal(reply.nodeRid, String(targetNodeRid));
          assert.equal(reply.marker, marker);
        }
        appendEvent(`outage-completed|nodeRid=${targetNodeRid}|messages=2`);
        break;
      default: throw new Error(`unsupported target outage command ${phase}`);
    }
  });
}

async function main() {
  if (mode !== 'user-spot-join-source'
    && mode !== 'user-spot-join-source-multiattempt'
    && mode !== 'user-spot-target') {
    throw new Error(`unsupported mode '${mode}'`);
  }
  if (mode === 'user-spot-target') {
    await userSpotJoinTarget();
    return;
  }
  await userSpotJoinSource();
}

main().catch(error => {
  const kind = error instanceof framework.ZLinkFrameworkException
    ? errorKindName(error.kind)
    : 'InternalFailure';
  appendEvent(`user-spot-join-failed|kind=${kind}|message=${String(error?.message ?? error)}`);
  console.error(`Node User-Spot Join source failed: ${error?.stack ?? error}`);
  process.exit(1);
});
