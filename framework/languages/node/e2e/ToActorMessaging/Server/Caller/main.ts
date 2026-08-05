import fs from 'node:fs';
import path from 'node:path';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException,
  ZLinkMessageFlowLogMode,
  type ZLinkActorClient,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import { ZLINK_ACTOR_CLIENT, ZLINK_LOCATION_RUNTIME_QUERY, ZLINK_ROUTE_MESH_RUNTIME, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { actorAsk, actorNotify, actorPush, type ActorCallRequest, type ActorCallResponse, type ActorReply } from '../../Shared/messages';
import { closeHttpServer, startHttpServer } from '../Support/http-server';
import { createLocationTopologyRoute } from '../Support/location-topology-route';
import { TO_ACTOR_OPTIONS, createToActorConfigurationModule } from '../../configuration';
import type { ServerOptions } from '../../configuration';

let options: ServerOptions;
let stopping = false;
const scenarioControls = new Set<string>();
const callTimeoutMs = 1000;

class CallerModule {}
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
        const builder = zlinkFramework();
        builder.addLocationStore(createRedisLocationStore({
          redisEndpoint: options.redisEndpoint,
          redisKeyPrefix: options.redisKeyPrefix
        }));
        locationMessagingOptions(builder.configureLocations());
        builder
          .configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(path.join(options.logDir, 'caller-flow.log'))
          .traceLabel(options.rid);
        const mesh = builder
          .addRouteMesh('to-actor')
          .listen(options.routerEndpoint).routingId(options.rid);
        mesh.objects().client();
        mesh.channel('to-actor').client();
        return builder.build();
      }
    })
  ]
})(CallerModule);

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(CallerModule, { logger: false, abortOnError: false });
  const actors = app.get(ZLINK_ACTOR_CLIENT, { strict: false }) as ZLinkActorClient;
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const routeMeshRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(options.httpUrl, [
    { method: 'GET', path: '/health', handle: () => ({ status: 'ok' }) },
    createLocationTopologyRoute(locations, routeMeshRuntime),
    {
      method: 'POST',
      path: '/send',
      handle: async (body) => {
        const request = body as ActorCallRequest;
        try {
          await actors
            .sendToActor(request.actorId, actorNotify(request.scenario, request.actorId, request.value))
            .submit();
          return { scenario: request.scenario, actorId: request.actorId, result: 'sent' } satisfies ActorCallResponse;
        } catch (error) {
          return failure(request, error);
        }
      }
    },
    {
      method: 'POST',
      path: '/request',
      handle: async (body) => {
        const request = body as ActorCallRequest;
        try {
          const reply = await actors
            .requestToActor(request.actorId, actorAsk(request.scenario, request.actorId, request.value))
            .timeout(callTimeoutMs)
            .submit<ActorReply>();
          return { scenario: request.scenario, actorId: request.actorId, result: reply.value } satisfies ActorCallResponse;
        } catch (error) {
          return failure(request, error);
        }
      }
    },
    {
      method: 'POST',
      path: '/push',
      handle: async (body) => {
        const request = body as ActorCallRequest;
        try {
          const reply = await actors
            .requestToActor(request.actorId, actorPush(request.scenario, request.actorId, request.value))
            .timeout(5000)
            .submit<ActorReply>();
          return { scenario: request.scenario, actorId: request.actorId, result: reply.value } satisfies ActorCallResponse;
        } catch (error) {
          return failure(request, error);
        }
      }
    },
    { method: 'GET', path: '/control/route-disconnected', handle: () => ({ ready: scenarioControls.has('route-disconnected') }) },
    { method: 'POST', path: '/control/route-disconnected', handle: () => { scenarioControls.add('route-disconnected'); return { ready: true }; } },
    { method: 'GET', path: '/control/route-restored', handle: () => ({ ready: scenarioControls.has('route-restored') }) },
    { method: 'POST', path: '/control/route-restored', handle: () => { scenarioControls.add('route-restored'); return { ready: true }; } },
    { method: 'POST', path: '/shutdown', handle: () => { stopping = true; return { status: 'stopping' }; } }
  ]);

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function failure(request: ActorCallRequest, error: unknown): ActorCallResponse {
  return {
    scenario: request.scenario,
    actorId: request.actorId,
    result: 'failed',
    errorKind: error instanceof ZLinkFrameworkException
      ? ZLinkFrameworkErrorKind[error.kind]
      : error instanceof Error
        ? error.name
        : String(error)
  };
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
