/* SPDX-License-Identifier: FSL-1.1-ALv2 */

const fs = require('node:fs');
const path = require('node:path');
const { Injectable, Module } = require('@nestjs/common');
const { NestFactory } = require('@nestjs/core');
const { logs } = require('@opentelemetry/api-logs');
const { LoggerProvider } = require('@opentelemetry/sdk-logs');

const framework = require('../packages/framework/dist');
const nestjs = require('../packages/nestjs/dist');
const locations = require('../packages/framework-locations-redis/dist');
const { decodeActorJoin28 } = require(
  '../packages/framework/dist/runtime/protocol/service_wire_pilot_codec.generated.js'
);

const CANONICAL_ACTOR_JOIN_PACKET = 'ZLinkFrameworkActorJoinRequest';
const mode = process.argv[2];
const args = parseArgs(process.argv.slice(3));

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
  if (stateful === undefined || typeof stateful.submitRequest !== 'function') {
    throw new Error('canonical Actor Join wire probe could not find the active Node service runtime');
  }

  const originalSubmitRequest = stateful.submitRequest;
  stateful.submitRequest = function submitRequestWithCanonicalProbe(
    pending,
    targetNodeRid,
    parts,
    timeoutMs,
    operationKind,
    actor
  ) {
    if (operationKind === 'actorJoin') {
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
    return originalSubmitRequest.call(
      this,
      pending,
      targetNodeRid,
      parts,
      timeoutMs,
      operationKind,
      actor
    );
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

class UserSpotActorCreateReq {
  constructor(stateVersion) { this.stateVersion = stateVersion; }
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
    const header = new TextEncoder().encode(JSON.stringify({
      stateVersion: actor.stateVersion,
      applicationStateBytes: actor.applicationState.byteLength
    }) + '\n');
    const payload = new Uint8Array(header.byteLength + actor.applicationState.byteLength);
    payload.set(header);
    payload.set(actor.applicationState, header.byteLength);
    return payload;
  }

  async restore(actor, payload, signal) {
    signal.throwIfAborted();
    const separator = payload.indexOf(10);
    if (separator < 0) throw new Error('relocation payload header is missing');
    const header = JSON.parse(new TextDecoder().decode(payload.subarray(0, separator)));
    actor.stateVersion = header.stateVersion;
    actor.applicationState = payload.slice(separator + 1);
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
    return {
      actorId: actor.actorId,
      spotId: String(actor.context.spotId),
      nodeRid: require_('node-rid'),
      stateVersion: actor.stateVersion,
      marker: request.marker
    };
  }
}
Injectable()(SourceProbeHandler);
nestjs.zlinkEntrySpotActorRequestHandler({
  actor: () => RelocationActor,
  entrySpot: () => RelocationEntrySpot,
  packetName: 'UserSpotProbeReq'
})(SourceProbeHandler);

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
        builder.addLocationStore(new locations.ZLinkRedisLocationStore({
          url: `redis://${require_('redis-endpoint')}`,
          keyPrefix: `${keyPrefix}:location`
        }));
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
        objects.addEntrySpot(RelocationEntrySpot);
        objects.addActorFactory(
          'cross-lang-relocation-actor-type',
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
    .getOrCreate(actorId, 'cross-lang-relocation-actor-type')
    .inMesh(meshName)
    .request(new UserSpotActorCreateReq(7))
    .timeout(15_000)
    .submit();
  appendEvent(`user-spot-source-actor-created|status=${created.status}|node=${created.actor?.nodeRid}`);

  const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT, { strict: false });
  const reply = await actorClient
    .requestToActor(actorId, new BeginUserSpotJoinReq(targetSpotId, 'canonical-28'))
    .timeout(45_000)
    .submit();
  appendEvent(
    `user-spot-join-request-reply|accepted=${reply.accepted === true}`
    + `|actor=${reply.actorId}|spot=${reply.targetSpotId}`
  );

  await new Promise(() => {});
  await app.close();
  await flowProvider?.shutdown();
}

async function main() {
  if (mode !== 'user-spot-join-source' && mode !== 'user-spot-join-source-multiattempt') {
    throw new Error(`unsupported mode '${mode}'`);
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
