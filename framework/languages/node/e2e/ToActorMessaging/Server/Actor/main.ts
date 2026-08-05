import fs from 'node:fs';
import path from 'node:path';
import { Inject, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  ZLinkMessage,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  type ZLinkActorCreateResponse,
  type ActorRef,
  type ZLinkActor,
  type ZLinkActorClient,
  type ZLinkActorContext,
  type ZLinkActorFactory,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkMessageContext,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_ACTOR_CLIENT,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLinkModule,
  zlinkEntrySpotActorRequestHandler,
  zlinkEntrySpotActorSendHandler,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type { ZLinkActorManager } from '@zlink-systems/framework';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import {
  PacketNames,
  type ActorAsk,
  type ActorNotify,
  ActorPushNotify,
  type ActorPushReq,
  type ActorRefPayload,
  type ActorReply
} from '../../Shared/messages';
import { EvidenceStore } from './evidence-store';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createLocationTopologyRoute } from '../Support/location-topology-route';
import { TO_ACTOR_OPTIONS, createToActorConfigurationModule } from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let evidence: EvidenceStore;
let stopping = false;

class TestActor implements ZLinkActor {
  constructor(
    readonly actorId: string,
    readonly context: ZLinkActorContext
  ) {}
}

class TestActorFactory implements ZLinkActorFactory {
  async create(context: ZLinkActorContext): Promise<TestActor> {
    return new TestActor(context.actorId, context);
  }
}

class TestEntrySpot implements ZLinkEntrySpot<TestActor> {
  readonly context!: ZLinkEntrySpotContext<TestActor>;
  private readonly actors = new Map<string, TestActor>();
  private readonly pendingDestroys = new Map<string, Promise<void>>();

  constructor(
    @Inject(ZLINK_ACTOR_CLIENT) private readonly actorClient: ZLinkActorClient
  ) {}

  async onCreateActor(actor: TestActor, _request: ZLinkMessage): Promise<ZLinkActorCreateResponse> {
    this.actors.set(actor.actorId, actor);
    evidence.append({ scenario: 'create', actorId: actor.actorId, kind: 'create', value: 'created' });
    return { accepted: true };
  }

  async onJoinedActor(_actor: TestActor): Promise<void> {}

  async onLeaveActor(_actor: TestActor): Promise<void> {}

  async onDisconnectActor(_actor: TestActor): Promise<void> {}

  async destroy(actorId: string): Promise<void> {
    const actor = this.actors.get(actorId);
    if (actor === undefined) {
      throw new Error(`Actor '${actorId}' is not active.`);
    }
    await this.actorClient
      .requestToActor(actorId, new DestroySelfReq())
      .submit<{ readonly scheduled: boolean }>();
    await this.pendingDestroys.get(actorId);
    this.pendingDestroys.delete(actorId);
    this.actors.delete(actorId);
  }

  scheduleDestroy(actor: TestActor): void {
    const operation = this.context.runIoWorker(async () => true).submit().then(async () => {
      await this.context.destroyActor(actor);
    });
    this.pendingDestroys.set(actor.actorId, operation);
  }
}

class DestroySelfReq {}

@zlinkEntrySpotActorRequestHandler({
  actor: () => TestActor,
  entrySpot: () => TestEntrySpot,
  packetName: 'DestroySelfReq'
})
class DestroySelfHandler {
  async handle(
    spot: TestEntrySpot,
    actor: TestActor,
    _context: ZLinkMessageContext,
    _request: DestroySelfReq
  ): Promise<{ readonly scheduled: boolean }> {
    spot.scheduleDestroy(actor);
    return { scheduled: true };
  }
}

@zlinkEntrySpotActorSendHandler({
  actor: () => TestActor,
  entrySpot: () => TestEntrySpot,
  packetName: PacketNames.actorNotify
})
class NotifyHandler {
  async handle(_spot: TestEntrySpot, actor: TestActor, _context: ZLinkMessageContext, message: ActorNotify): Promise<void> {
    evidence.append({ scenario: message.scenario, actorId: actor.actorId, kind: 'send', value: message.value });
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => TestActor,
  entrySpot: () => TestEntrySpot,
  packetName: PacketNames.actorAsk
})
class AskHandler {
  async handle(_spot: TestEntrySpot, actor: TestActor, _context: ZLinkMessageContext, request: ActorAsk): Promise<ActorReply> {
    if (request.value === 'throw') {
      throw new Error('to-actor handler exception');
    }
    evidence.append({ scenario: request.scenario, actorId: actor.actorId, kind: 'request', value: request.value });
    return { scenario: request.scenario, actorId: actor.actorId, value: `reply:${request.value}` };
  }
}

@zlinkEntrySpotActorRequestHandler({
  actor: () => TestActor,
  entrySpot: () => TestEntrySpot,
  packetName: PacketNames.actorPush
})
class PushHandler {
  async handle(_spot: TestEntrySpot, actor: TestActor, _context: ZLinkMessageContext, request: ActorPushReq): Promise<ActorReply> {
    await actor.context.boundSession
      .send(new ActorPushNotify(request.scenario, actor.actorId, request.value))
      .submit();
    evidence.append({ scenario: request.scenario, actorId: actor.actorId, kind: 'push', value: request.value });
    return { scenario: request.scenario, actorId: actor.actorId, value: `pushed:${request.value}` };
  }
}

class ActorModule {}
const configuration = createToActorConfigurationModule();
Module({
  imports: [
    configuration,
    ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [TO_ACTOR_OPTIONS],
      useFactory: (value: unknown) => {
        options = value as ServerOptions;
        fs.mkdirSync(options.logDir, { recursive: true });
        evidence = new EvidenceStore(options.evidenceFile ?? path.join(options.logDir, 'actor.evidence.log'));
        const builder = zlinkFramework();
        builder
          .addLocationStore(createRedisLocationStore({
            redisEndpoint: options.redisEndpoint,
            redisKeyPrefix: options.redisKeyPrefix
          }));
        locationMessagingOptions(builder.configureLocations());
        builder
          .configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(path.join(options.logDir, 'actor-flow.log'))
          .traceLabel(options.rid);
        const mesh = builder
          .addRouteMesh('to-actor')
          .listen(options.routerEndpoint).routingId(options.rid);
        const objectServer = mesh.objects().server();
        objectServer.addEntrySpot(TestEntrySpot);
        objectServer.addActorFactory(
          'test-actor',
          TestActorFactory,
          (factory) => factory.disableRelocation()
        );
        mesh.channel('to-actor').server();
        return builder.build();
      }
    })
  ],
  providers: [TestActorFactory, TestEntrySpot, NotifyHandler, AskHandler, PushHandler, DestroySelfHandler]
})(ActorModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(ActorModule, { logger: false, abortOnError: false });
  const actors = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as ZLinkActorManager;
  const entrySpot = app.get(TestEntrySpot, { strict: false });
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok' }) },
    createLocationTopologyRoute(locations, routeMeshRuntime),
    { method: 'GET', path: '/evidence', handle: () => evidence.all() },
    {
      method: 'POST',
      path: '/actors/ta-a1/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-a1');
        return { actorId: 'ta-a1', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-a2/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-a2');
        return { actorId: 'ta-a2', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-a3/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-a3');
        return { actorId: 'ta-a3', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-a4/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-a4');
        return { actorId: 'ta-a4', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b1-reference/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-b1-reference');
        return { actorId: 'ta-b1-reference', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-a4/destroy',
      handle: async () => {
        await entrySpot.destroy('ta-a4');
        return { actorId: 'ta-a4', status: 'destroyed' };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b1-reference/destroy',
      handle: async () => {
        await entrySpot.destroy('ta-b1-reference');
        return { actorId: 'ta-b1-reference', status: 'destroyed' };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b2/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-b2');
        return { actorId: 'ta-b2', actor: actorSnapshot(actor) };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b2/destroy',
      handle: async () => {
        await entrySpot.destroy('ta-b2');
        return { actorId: 'ta-b2', status: 'destroyed' };
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b2/destroy-ref',
      handle: async (body) => {
        try {
          const destroyed = await actors.destroy(actorRefFromSnapshot(body as ActorRefPayload));
          return { actorId: 'ta-b2', status: destroyed ? 'destroyed' : 'not-found' };
        } catch (error) {
          return {
            actorId: 'ta-b2',
            status: 'failed',
            errorKind: error instanceof ZLinkFrameworkException
              ? ZLinkFrameworkErrorKind[error.kind]
              : error instanceof Error
                ? error.name
                : String(error)
          };
        }
      }
    },
    {
      method: 'POST',
      path: '/actors/ta-b3/ensure',
      handle: async () => {
        const actor = await ensureActor(actors, 'ta-b3');
        return { actorId: 'ta-b3', actor: actorSnapshot(actor) };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function actorSnapshot(actor: ActorRef): ActorRefPayload {
  return {
    nodeRid: String(actor.nodeRid),
    actorId: actor.actorId,
    objectGeneration: actor.objectGeneration.toString(),
    meshName: actor.meshName
  };
}

function actorRefFromSnapshot(actor: ActorRefPayload): ActorRef {
  return {
    nodeRid: actor.nodeRid,
    actorId: actor.actorId,
    objectGeneration: BigInt(actor.objectGeneration),
    meshName: actor.meshName
  };
}

async function ensureActor(actors: ZLinkActorManager, actorId: string): Promise<ActorRef> {
  const result = await actors
    .getOrCreate(actorId, 'test-actor')
    .inMesh('to-actor')
    .submit();
  if (result.status === 'rejected') {
    throw new Error(`Actor '${actorId}' creation was rejected.`);
  }
  return result.actor;
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
