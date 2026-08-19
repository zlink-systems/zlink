/* SPDX-License-Identifier: FSL-1.1-ALv2 */

/* Node peer host for the C++ cross-language smoke (G6).
 *
 * Same role modes as the C++ host and the .NET TestHost, built on the public
 * Node packages, so each C++↔Node direction runs over the real wire. Writes a
 * ready file once the role is up and appends markers to the event file. */

const fs = require('node:fs');
const path = require('node:path');
const { pathToFileURL } = require('node:url');
/* The Node packages and their peer dependencies resolve from the Node
 * workspace, not from this directory. */
const nodeRoot = path.resolve(__dirname, '../../node');
const { Injectable, Module } = require(path.join(nodeRoot, 'node_modules/@nestjs/common'));
const { NestFactory } = require(path.join(nodeRoot, 'node_modules/@nestjs/core'));
const nestjs = require(path.join(nodeRoot, 'packages/nestjs/dist'));
const framework = require(path.join(nodeRoot, 'packages/framework/dist'));

const args = parseArgs(process.argv.slice(3));
const mode = process.argv[2];

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
  if (!value) {
    throw new Error(`--${name} is required`);
  }
  return value;
}

function appendEvent(line) {
  console.log(line);
  if (args['event-file']) {
    fs.appendFileSync(args['event-file'], `${line}\n`);
  }
}

function writeReady() {
  if (args['ready-file']) {
    fs.writeFileSync(args['ready-file'], 'ready\n');
  }
}

class TestHostProfileRequest {
  constructor(value) { this.value = value; }
}
class TestHostProfileSend {
  constructor(value) { this.value = value; }
}
class TestHostPublishedEvent {
  constructor(value) { this.value = value; }
}

function readValue(payload) {
  return payload?.value ?? payload?.Value;
}

async function channelServer() {
  class ProfileRequestHandler {
    async handle(payload) {
      appendEvent(`channel-server-request|${readValue(payload)}`);
      return { value: readValue(payload) };
    }
  }
  Injectable()(ProfileRequestHandler);
  class ProfileSendHandler {
    async handle(payload) {
      appendEvent(`channel-server-send|${readValue(payload)}`);
    }
  }
  Injectable()(ProfileSendHandler);

  class ServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        /* RouteMesh 10.x (1de8f43917) replaced enableServer(endpoint) with the
         * server().setBindHost(host).listen(port) role builder. */
        const serverEndpoint = new URL(require_('server-endpoint'));
        builder.addClientServerChannel(require_('channel-name'))
          .server()
          .setBindHost(serverEndpoint.hostname)
          .listen(Number(serverEndpoint.port))
          .addRequestHandler('TestHostProfileRequest', ProfileRequestHandler)
          .addSendHandler('TestHostProfileSend', ProfileSendHandler);
        return builder.build();
      }
    })],
    providers: [ProfileRequestHandler, ProfileSendHandler]
  })(ServerModule);

  const app = await NestFactory.createApplicationContext(ServerModule, { logger: false });
  writeReady();
  await new Promise(() => {});
  await app.close();
}

async function channelClient() {
  class ClientModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => nestjs.zlinkFramework()
        /* RouteMesh 10.x (1de8f43917): enableClient(endpoint) is now
         * client().connect(endpoint). */
        .addClientServerChannel(require_('channel-name'))
        .client()
        .connect(require_('server-endpoint'))
        .build()
    })]
  })(ClientModule);

  const app = await NestFactory.createApplicationContext(ClientModule, { logger: false });
  const client = app.get(nestjs.ZLINK_CHANNEL_CLIENT, { strict: false });
  const value = args.value ?? 'node-to-cpp';
  const reply = await client
    .requestToChannel(require_('channel-name'), new TestHostProfileRequest(value))
    .timeout(5000)
    .submit();
  appendEvent(`channel-client-reply|${readValue(reply)}`);
  await client
    .sendToChannel(require_('channel-name'), new TestHostProfileSend(`${value}-send`))
    .submit();
  appendEvent(`channel-client-sent|${value}-send`);
  writeReady();
  await new Promise(() => {});
}

async function channelSubscriber() {
  class PublishedEventHandler {
    async handle(payload, context) {
      appendEvent(`${context.topic}:${readValue(payload)}`);
    }
  }
  Injectable()(PublishedEventHandler);

  class SubscriberModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addFanoutChannel(require_('channel-name'))
          .enableSubscriber(require_('publisher-endpoint'))
          .addPublishHandler('TestHostPublishedEvent', PublishedEventHandler);
        return builder.build();
      }
    })],
    providers: [PublishedEventHandler]
  })(SubscriberModule);

  const app = await NestFactory.createApplicationContext(SubscriberModule, { logger: false });
  writeReady();
  await new Promise(() => {});
  await app.close();
}

async function channelPublisher() {
  class PublisherModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => nestjs.zlinkFramework()
        .addFanoutChannel(require_('channel-name'))
        .enablePublisher(require_('publisher-endpoint'))
        .build()
    })]
  })(PublisherModule);

  const app = await NestFactory.createApplicationContext(PublisherModule, { logger: false });
  const publisher = app.get(nestjs.ZLINK_FANOUT_CLIENT, { strict: false });
  const topic = require_('topic');
  const value = args.value ?? 'node-publish';
  writeReady();
  /* A subscriber may still be connecting: repeat like the peer publishers do. */
  for (;;) {
    await publisher.publish(require_('channel-name'), topic, new TestHostPublishedEvent(value)).submit();
    await new Promise((resolve) => setTimeout(resolve, 200));
  }
}

/* Snake_case error-code table (mirrors the shared 13-name wire table) so the
 * recorded client markers use the wire names. */
const ERROR_KIND_WIRE_NAMES = [
  'not_found', 'already_exists', 'type_mismatch', 'not_configured', 'rejected',
  'unavailable', 'capacity_exceeded', 'deadline_exceeded', 'shutting_down',
  'protocol_error', 'invalid_operation', 'data_lost', 'internal_failure'
];

function errorKindWireName(kind) {
  return ERROR_KIND_WIRE_NAMES[kind] ?? 'internal_failure';
}

function errorOriginWireName(error) {
  return typeof error.origin === 'string' ? error.origin : 'none';
}

class TestHostSpotRouteRequest {
  constructor(value) { this.value = value; }
}
class TestHostSpotRouteFailRequest {
  constructor(value) { this.value = value; }
}
class TestHostSpotRouteMissingRequest {
  constructor(value) { this.value = value; }
}

/* Spot route wire host: (a) echo handler tagged "|node" and (c) an
 * application handler failing with the typed rejected kind. */
async function spotRouteServer() {
  /* Unlike ClientServerChannel's enableServer(), RouteMesh's listen() path
   * does not hold an active libuv handle in the Node backend: its native
   * completions are not libuv handles, so with nothing else referencing the
   * event loop, the loop drains and the process exits (code 0) right after
   * writeReady() despite the never-resolving `await new Promise(() => {})`
   * below. Same keep-alive fix as spotRouteClient(). */
  setInterval(() => {}, 1000);
  class SpotRouteRequestHandler {
    async handle(payload) {
      appendEvent(`spot-route-server|${readValue(payload)}`);
      return { value: `${readValue(payload)}|node` };
    }
  }
  Injectable()(SpotRouteRequestHandler);
  class SpotRouteFailHandler {
    async handle() {
      throw new framework.ZLinkFrameworkException(
        framework.ZLinkFrameworkErrorKind.Rejected, 'application spot route failure');
    }
  }
  Injectable()(SpotRouteFailHandler);

  class SpotRouteServerModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        builder.addRouteMesh(require_('channel-name'))
          .listen(require_('server-endpoint'))
          .routingId(args['node-rid'] ?? 'node-spot-route')
          .addRequestHandler('TestHostSpotRouteRequest', SpotRouteRequestHandler)
          .addRequestHandler('TestHostSpotRouteFailRequest', SpotRouteFailHandler)
          .channel(require_('channel-name')).server();
        return builder.build();
      }
    })],
    providers: [SpotRouteRequestHandler, SpotRouteFailHandler]
  })(SpotRouteServerModule);

  const app = await NestFactory.createApplicationContext(SpotRouteServerModule, { logger: false });
  writeReady();
  await new Promise(() => {});
  await app.close();
}

/* Spot route wire client: common (a)/(b)/(c) scenario against a peer host,
 * recording the observed wire kind and origin marker. */
async function spotRouteClient() {
  const channel = require_('channel-name');
  const peerRid = require_('peer-rid');
  /* The framework's native completions are not libuv handles; without a
   * keep-alive timer the event loop can drain mid-request and the process
   * exits silently. */
  setInterval(() => {}, 1000);
  class SpotRouteClientModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        const mesh = builder.addRouteMesh(channel)
          .listen(require_('bind-endpoint'))
          .routingId(args['node-rid'] ?? 'node-spot-route-client');
        mesh.channel(channel).client();
        mesh.peerConnections().connect(peerRid, require_('server-endpoint'));
        return builder.build();
      }
    })]
  })(SpotRouteClientModule);

  const app = await NestFactory.createApplicationContext(SpotRouteClientModule, { logger: false });
  const routeMeshRuntime = app.get(nestjs.ZLINK_ROUTE_MESH_RUNTIME, { strict: false });
  const deadline = Date.now() + 15_000;
  while (routeMeshRuntime.snapshot(channel).readyPeerCount === 0) {
    if (Date.now() >= deadline) {
      throw new Error('spot route peer readiness timed out');
    }
    await new Promise((resolve) => setTimeout(resolve, 25));
  }
  const client = app.get(nestjs.ZLINK_ROUTE_CLIENT, { strict: false });
  const value = args.value ?? 'node-to-peer-spot-route';

  let reply;
  try {
    reply = await client
      .requestToNode(channel, peerRid, new TestHostSpotRouteRequest(value))
      .timeout(5000).submit();
  } catch (error) {
    /* Terminal (a)-failure: recorded so the runner can pin known
     * cross-language divergences (e.g. the C++/Java service-wire reply
     * header without the u16 tail field fails decode here). */
    appendEvent(
      `spot-route-error|kind=${errorKindWireName(error.kind)}|origin=${errorOriginWireName(error)}`);
    writeReady();
    await new Promise(() => {});
  }
  appendEvent(`spot-route-reply|${readValue(reply)}`);

  const recordFailure = async (marker, request) => {
    try {
      await client.requestToNode(channel, peerRid, request).timeout(5000).submit();
      appendEvent(`${marker}|unexpected-success`);
    } catch (error) {
      appendEvent(
        `${marker}|kind=${errorKindWireName(error.kind)}|origin=${errorOriginWireName(error)}`);
    }
  };
  await recordFailure('spot-route-missing', new TestHostSpotRouteMissingRequest(value));
  await recordFailure('spot-route-app-error', new TestHostSpotRouteFailRequest(value));
  writeReady();
  await new Promise(() => {});
}

/* Entry-spot actor relocation: two nodes (one of them this Node peer, the
 * other a peer-language host) share a Redis-backed Location/Relocation
 * Store, both register the SAME entry spot + actor factory, the source
 * creates an actor (entering the entry spot the same way JoinEntrySpot
 * does -- no admission roundtrip, spec 15:489), then triggers a whole-node
 * relocate() drain. With exactly one other eligible peer in the mesh the
 * destination is deterministic. A post-relocation probe request confirms
 * the actor answers on the target node (owner transition). */
async function entrySpotRelocate() {
  const role = require_('role');
  if (role !== 'source' && role !== 'target') {
    throw new Error(`--role must be 'source' or 'target', got '${role}'`);
  }
  const meshName = require_('mesh-name');
  const nodeRid = require_('node-rid');
  const peerRid = require_('peer-rid');
  const actorId = args['actor-id'] ?? 'cross-lang-relocation-actor';
  const actorType = 'cross-lang-relocation-actor-type';
  const payloadBytes = Number(args['payload-bytes'] ?? 100000);
  const keyPrefix = args['redis-key-prefix'] ?? 'zlink-cross-relocation';

  /* Same RouteMesh keep-alive gotcha as spotRouteServer()/spotRouteClient(). */
  setInterval(() => {}, 1000);

  const locations = require(path.join(nodeRoot, 'packages/framework-locations-redis/dist'));

  function deterministicState(byteLength) {
    const state = new Uint8Array(byteLength);
    for (let index = 0; index < byteLength; index += 1) {
      state[index] = index % 251;
    }
    return state;
  }

  class RelocationActor {
    constructor(actorId, context) {
      this.actorId = actorId;
      this.context = context;
      this.applicationState = new Uint8Array();
      this.stateVersion = 0;
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
      const encoded = new Uint8Array(header.byteLength + actor.applicationState.byteLength);
      encoded.set(header);
      encoded.set(actor.applicationState, header.byteLength);
      return encoded;
    }

    async restore(actor, payload, signal) {
      signal.throwIfAborted();
      const separator = payload.indexOf(10);
      if (separator < 0) throw new Error('relocation payload header is missing');
      const header = JSON.parse(new TextDecoder().decode(payload.subarray(0, separator)));
      actor.stateVersion = header.stateVersion;
      actor.applicationState = payload.slice(separator + 1);
      if (actor.applicationState.byteLength !== header.applicationStateBytes) {
        throw new Error('relocation application state size changed during restore');
      }
    }
  }
  Injectable()(RelocationActorAdapter);

  class RelocationEntrySpot {
    async onCreateActor(actor, request) {
      const create = request.toEncodedPayload().isEmpty() ? {} : request.decode();
      actor.stateVersion = create.stateVersion ?? 1;
      actor.applicationState = deterministicState(create.applicationStateBytes ?? 0);
      return { accepted: true };
    }

    async onJoinedActor() {}

    async onLeaveActor() {}

    async onDisconnectActor() {}
  }
  Injectable()(RelocationEntrySpot);

  class CrossLangProbeReq {
    constructor(marker) { this.marker = marker; }
  }

  class EntryProbeHandler {
    async handle(spot, actor, context, request) {
      appendEvent(`entry-spot-probe-served|node=${nodeRid}|actor=${actor.actorId}`);
      return {
        nodeRid,
        stateVersion: actor.stateVersion,
        applicationStateBytes: actor.applicationState.byteLength,
        marker: request.marker
      };
    }
  }
  nestjs.zlinkEntrySpotActorRequestHandler({
    actor: () => RelocationActor,
    entrySpot: () => RelocationEntrySpot,
    packetName: 'CrossLangProbeReq'
  })(EntryProbeHandler);

  class EntrySpotRelocationModule {}
  Module({
    imports: [nestjs.ZLinkModule.forRootFactory({
      useFactory: () => {
        const builder = nestjs.zlinkFramework();
        const store = new locations.ZLinkRedisLocationStore({
          url: `redis://${require_('redis-endpoint')}`,
          keyPrefix: `${keyPrefix}:location`
        });
        const relocationStore = new locations.ZLinkRedisRelocationStore({
          url: `redis://${require_('redis-endpoint')}`,
          keyPrefix: `${keyPrefix}:relocation`
        });
        builder.addLocationStore(store);
        builder.addRelocationStore(relocationStore);
        const mesh = builder.addRouteMesh(meshName)
          .listen(require_('bind-endpoint'))
          .routingId(nodeRid)
          /* Force deterministic placement: the source always wins actor
           * creation, so the pre-relocation owner assertion is meaningful
           * rather than an accident of the placement algorithm. */
          .setPlacementWeight(role === 'source' ? 100 : 0);
        /* No manual peerConnections().connect() at all: .NET's relocate()
         * explicitly rejects manual topology with
         * ZLinkFrameworkRelocationReason.ManualTopologyUnsupported --
         * confirmed by direct repro (relocate() returned
         * outcome=Blocked|reason=ManualTopologyUnsupported the moment a
         * PeerConnections.Connect(...) call was present on the .NET side).
         * relocate() only works under pure automatic discovery: both nodes
         * register the same shared Location Store and NEITHER calls
         * PeerConnections.Connect; peers are supposed to find each other
         * through the store alone. */
        const objects = mesh.objects().server();
        objects.addEntrySpot(RelocationEntrySpot);
        objects.addActorFactory(actorType, RelocationActorFactory,
          (factory) => factory.preserveStateWith(RelocationActorAdapter));
        return builder.build();
      }
    })],
    providers: [RelocationActorFactory, RelocationActorAdapter, RelocationEntrySpot, EntryProbeHandler]
  })(EntrySpotRelocationModule);

  const app = await NestFactory.createApplicationContext(EntrySpotRelocationModule, { logger: false });
  /* No manual peerConnections().connect() anywhere in this mode (see the
   * comment above), so there is no outbound connection to poll readiness
   * on -- automatic discovery converges through the shared Location Store
   * on its own schedule; actor-create/relocate below carry their own
   * timeouts and retries. */
  writeReady();

  if (role === 'source') {
    const actorManager = app.get(nestjs.ZLINK_ACTOR_MANAGER, { strict: false });
    const createResult = await actorManager
      .getOrCreate(actorId, actorType)
      .inMesh(meshName)
      .request({ stateVersion: 1, applicationStateBytes: payloadBytes })
      .timeout(15000)
      .submit();
    appendEvent(`entry-spot-create|status=${createResult.status}`);
    appendEvent(`entry-spot-owner-before|node=${createResult.actor?.nodeRid}`);

    const frameworkRuntime = app.get(nestjs.ZLINK_FRAMEWORK_RUNTIME, { strict: false });
    /* No manual connect means no readiness signal to wait on before
     * relocating -- give automatic discovery (Location Store polling on
     * both ends) time to converge, retrying relocate() itself a few times
     * since an early attempt can observe zero eligible peers yet. */
    let relocateResult;
    for (let attempt = 0; attempt < 6; attempt += 1) {
      await new Promise((resolve) => setTimeout(resolve, 5000));
      relocateResult = await frameworkRuntime.relocate({
        mode: framework.ZLinkFrameworkRelocationMode.PlannedMaintenance,
        deadlineMs: 30000
      });
      appendEvent(
        `relocate-attempt|attempt=${attempt}`
        + `|outcome=${framework.ZLinkFrameworkRelocationOutcome[relocateResult.outcome]}`
        + `|reason=${framework.ZLinkFrameworkRelocationReason[relocateResult.reason]}`
      );
      if (relocateResult.outcome === framework.ZLinkFrameworkRelocationOutcome.Relocated) break;
    }
    appendEvent(
      `relocate-result|outcome=${framework.ZLinkFrameworkRelocationOutcome[relocateResult.outcome]}`
      + `|reason=${framework.ZLinkFrameworkRelocationReason[relocateResult.reason]}`
    );
  } else {
    const actorClient = app.get(nestjs.ZLINK_ACTOR_CLIENT, { strict: false });
    const probeDeadline = Date.now() + 60_000;
    let lastReply;
    for (;;) {
      try {
        lastReply = await actorClient
          .requestToActor(actorId, new CrossLangProbeReq('post-relocate-probe'))
          .timeout(5000)
          .submit();
        if (lastReply.nodeRid === nodeRid) break;
      } catch {
        /* Actor may still be mid-relocation or not yet created; keep polling. */
      }
      if (Date.now() >= probeDeadline) {
        throw new Error(`entry-spot-relocate target probe deadline exceeded, last reply=${JSON.stringify(lastReply)}`);
      }
      await new Promise((resolve) => setTimeout(resolve, 500));
    }
    appendEvent(
      `entry-spot-probe|nodeRid=${lastReply.nodeRid}|stateVersion=${lastReply.stateVersion}`
      + `|applicationStateBytes=${lastReply.applicationStateBytes}`
    );
  }

  await new Promise(() => {});
}

async function streamConnector() {
  const driverModule = await import(pathToFileURL(
    path.join(nodeRoot, 'scripts/browser-e2e/connector-driver.mjs')
  ).href);
  const instance = await driverModule.createBrowserConnectorDriver();
  await instance.connect(require_('stream-endpoint'));
  const value = args.value ?? 'node-connector-to-cpp';
  const reply = await instance.request('RawPing', value, true);
  appendEvent(`connector-reply|${typeof reply === 'string' ? reply : JSON.stringify(reply)}`);
  writeReady();
  await instance.close();
}

function messageFollowRoute(
  targetNodeRid,
  actorNodeRid,
  targetNodeGeneration,
  authorityOwnerGeneration
) {
  return {
    kind: 'actor',
    actor: {
      nodeRid: actorNodeRid,
      actorId: 'cross-language-message-follow',
      generation: 1n
    },
    targetNodeRid,
    targetNodeGeneration,
    authorityOwnerGeneration,
    ownerLeaseGeneration: 8n
  };
}

async function messageFollow() {
  const { ZLinkNodeRawMeshBackend } = require(path.join(
    nodeRoot,
    'packages/framework/dist/runtime/backend/node/node-raw-mesh-backend.js'
  ));
  /* The raw backend constructor now requires an explicit binding port and the
   * host Application Job Queue (ownership-alignment work); mirror the m6a/m6b
   * contract tests' standalone construction. */
  const { ZLinkNodeRawBindingPort } = require(path.join(
    nodeRoot,
    'packages/framework/dist/runtime/backend/node/node-raw-binding-port.js'
  ));
  const { ApplicationJobQueue, resolveApplicationJobQueueConfiguration } = require(path.join(
    nodeRoot,
    'packages/framework/dist/runtime/host/application-job-queue.js'
  ));
  const localRid = require_('node-rid');
  const peerRid = require_('peer-rid');
  const backend = new ZLinkNodeRawMeshBackend(
    'message-follow',
    localRid,
    new ZLinkNodeRawBindingPort(),
    new ApplicationJobQueue(
      resolveApplicationJobQueueConfiguration({ maxQueuedApplicationJobs: 2_048n }, () => 1n)
    )
  );
  let sent = false;
  let received = false;
  let finished = false;
  backend.setBind(require_('bind-endpoint'));
  backend.setMessageFollowHandler((record) => {
    appendEvent(
      `message-follow-received|source-node=${record.source.targetNodeRid}`
      + `|target-node=${record.target.targetNodeRid}`
      + `|operation-low=${record.originalOperation.low.toString()}`
    );
    received = true;
  });
  backend.start();
  backend.connectPeer({
    endpoint: require_('peer-endpoint'),
    expectedRid: peerRid,
    expectedLifecycleGeneration: 1n
  });
  writeReady();

  const finish = (code, error) => {
    if (finished) return;
    finished = true;
    if (error) console.error(`node message-follow failed: ${error}`);
    backend.close();
    process.exitCode = code;
  };
  const deadline = Date.now() + 60_000;
  const timer = setInterval(() => {
    try {
      const peer = backend.peers().find(
        (entry) => String(entry.routingId) === peerRid
      );
      const routeReady = peer !== undefined
        && backend.isPeerRouteReady(peerRid, peer.lifecycleGeneration);
      if (!sent && routeReady) {
        const local = backend.status();
        backend.sendMessageFollowNotification(peerRid, {
          source: messageFollowRoute(
            localRid,
            localRid,
            local.lifecycleGeneration,
            7n
          ),
          target: messageFollowRoute(
            peerRid,
            localRid,
            peer.lifecycleGeneration,
            8n
          ),
          hopCount: 1,
          queuedMessages: 1,
          queuedBytes: 64,
          originalOperation: { high: 3n, low: 202n },
          originalReplyRouteId: 11n
        });
        appendEvent('message-follow-sent|operation-low=202');
        sent = true;
      }
      if (sent && received) {
        clearInterval(timer);
        finish(0);
      } else if (Date.now() >= deadline) {
        clearInterval(timer);
        finish(1, new Error(`exchange timed out (sent=${sent}, received=${received})`));
      }
    } catch (error) {
      clearInterval(timer);
      finish(1, error);
    }
  }, 2);
}

async function main() {
  switch (mode) {
    case 'channel-server': return channelServer();
    case 'channel-client': return channelClient();
    case 'channel-subscriber': return channelSubscriber();
    case 'channel-publisher': return channelPublisher();
    case 'spot-route-server': return spotRouteServer();
    case 'spot-route-client': return spotRouteClient();
    case 'browser-stream-connector': return streamConnector();
    case 'message-follow': return messageFollow();
    case 'entry-spot-relocate': return entrySpotRelocate();
    default: throw new Error(`unsupported mode '${mode}'`);
  }
}

main().catch((error) => {
  console.error(`node peer host failed: ${error?.stack ?? error}`);
  process.exit(1);
});
