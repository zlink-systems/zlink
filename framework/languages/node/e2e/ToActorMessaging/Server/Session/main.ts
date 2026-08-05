import fs from 'node:fs';
import path from 'node:path';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  type ZLinkMessage,
  type ZLinkSession,
  type ZLinkSessionContext,
  type ZLinkSessionDispatchContext,
  type ZLinkSessionFactory,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import { ZLINK_LOCATION_RUNTIME_QUERY, ZLINK_ROUTE_MESH_RUNTIME, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import {
  PacketNames,
  type ActorPushReq,
  type BindActorReq,
  type BindActorRes,
  type SessionBindingSnapshot
} from '../../Shared/messages';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createLocationTopologyRoute } from '../Support/location-topology-route';
import { TO_ACTOR_OPTIONS, createToActorConfigurationModule } from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let stopping = false;

class SessionBindingRegistry {
  private readonly sessionByActor = new Map<string, string>();

  bind(actorId: string, sessionId: string): void {
    this.sessionByActor.set(actorId, sessionId);
  }

  disconnect(sessionId: string): void {
    for (const [actorId, boundSessionId] of this.sessionByActor) {
      if (boundSessionId === sessionId) {
        this.sessionByActor.delete(actorId);
      }
    }
  }

  snapshot(actorId: string): SessionBindingSnapshot {
    const sessionId = this.sessionByActor.get(actorId);
    return { actorId, sessionIds: sessionId === undefined ? [] : [sessionId] };
  }
}

const sessionBindings = new SessionBindingRegistry();

class ToActorSession implements ZLinkSession {
  constructor(readonly context: ZLinkSessionContext) {}

  async onDispatch(dispatch: ZLinkSessionDispatchContext, payload: ZLinkMessage, signal?: AbortSignal): Promise<void> {
    if (dispatch.packetName === PacketNames.bindActor) {
      const request = payload.decode<BindActorReq>(Object as never);
      await this.context.actors.bindOrGet({
        actorId: request.actor.actorId,
        nodeRid: request.actor.nodeRid,
        objectGeneration: BigInt(request.actor.objectGeneration),
        meshName: request.actor.meshName
      }, signal);
      sessionBindings.bind(request.actor.actorId, this.context.sessionId);
      this.context.client.reply({
        actorId: request.actor.actorId,
        nodeRid: request.actor.nodeRid,
        objectGeneration: request.actor.objectGeneration,
        boundCount: this.context.actors.bound.length
      } satisfies BindActorRes).submit();
      return;
    }

    if (dispatch.packetName === PacketNames.actorPush) {
      const request = payload.decode<ActorPushReq>(Object as never);
      const actor = this.context.actors.find(request.actorId);
      if (actor === undefined) {
        throw new Error(`Actor route not found: ${request.actorId}`);
      }
      await actor.relay(payload, signal);
      return;
    }

    throw new Error(`Unsupported session packet '${dispatch.packetName}'.`);
  }

  async onDisconnected(): Promise<void> {
    sessionBindings.disconnect(this.context.sessionId);
  }
}

@Injectable()
class ToActorSessionFactory implements ZLinkSessionFactory<ToActorSession> {
  async create(context: ZLinkSessionContext): Promise<ToActorSession> {
    return new ToActorSession(context);
  }
}

class SessionModule {}
const configuration = createToActorConfigurationModule();
Module({
  imports: [
    configuration,
    ZLinkModule.forRootFactory({
      imports: [configuration],
      inject: [TO_ACTOR_OPTIONS],
      useFactory: (value: unknown) => {
        options = value as ServerOptions;
        if (options.streamEndpoint === undefined) {
          throw new Error("Configuration value 'e2e.streamEndpoint' is required for the session host.");
        }
        fs.mkdirSync(options.logDir, { recursive: true });
        const builder = zlinkFramework();
        builder.addLocationStore(createRedisLocationStore({
          redisEndpoint: options.redisEndpoint,
          redisKeyPrefix: options.redisKeyPrefix
        }));
        locationMessagingOptions(builder.configureLocations());
        builder
          .configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(path.join(options.logDir, 'session-flow.log'))
          .traceLabel(options.rid);
        const mesh = builder
          .addRouteMesh('to-actor')
          .listen(options.routerEndpoint).routingId(options.rid);
        mesh.objects().client();
        mesh.channel('to-actor').server();
        builder
          .addStreamNode('to-actor-session-stream')
          .bind(options.streamEndpoint)
          .enableActorDispatch()
          .registerSession(ToActorSessionFactory);
        return builder.build();
      }
    })
  ],
  providers: [ToActorSessionFactory]
})(SessionModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(SessionModule, { logger: false, abortOnError: false });
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok' }) },
    createLocationTopologyRoute(locations, routeMeshRuntime),
    {
      method: 'POST',
      path: '/bindings/snapshot',
      handle: (body) => sessionBindings.snapshot((body as { readonly actorId: string }).actorId)
    },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
