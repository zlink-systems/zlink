import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  type ZLinkFrameworkRuntime,
  type ZLinkEntrySpot,
  type ZLinkEntrySpotContext,
  type ZLinkMessage,
  type ZLinkSpotActorJoinResult
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { ZLinkModule, ZLINK_FRAMEWORK_RUNTIME, zlinkFramework } from '@zlink-systems/nestjs';
import { AutomaticTurnDispatchNames } from '../../Shared/messages';
import { EvidenceStore } from './Support/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { createAutomaticTurnConfigurationModule } from '../../configuration';
import { SESSION_OPTIONS, validateSessionOptions } from './Configuration/session-options';
import type { SessionOptions } from './Configuration/session-options';
import { AwaitSessionFactory } from './Handlers/await-session';

class AwaitSessionEntrySpot implements ZLinkEntrySpot {
  readonly context!: ZLinkEntrySpotContext;

  async onActorJoin(actorId: string, request: ZLinkMessage): Promise<ZLinkSpotActorJoinResult> {
    void actorId;
    void request;
    return { accepted: true };
  }

  async onJoinedActor(actor: import('@zlink-systems/framework').ZLinkActor): Promise<void> { void actor; }

  async onLeaveActor(actor: import('@zlink-systems/framework').ZLinkActor): Promise<void> { void actor; }

  async onDisconnectActor(actor: import('@zlink-systems/framework').ZLinkActor): Promise<void> { void actor; }
}

export async function startSessionHost(): Promise<void> {
  let stopping = false;
  const configuration = createAutomaticTurnConfigurationModule(SESSION_OPTIONS, validateSessionOptions);

  class SessionModule {}
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [SESSION_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as SessionOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const locationStore = new ZLinkRedisLocationStore({
            url: `redis://${options.redisEndpoint}`,
            keyPrefix: options.redisKeyPrefix
          });
          const builder = zlinkFramework();
          builder
            .addLocationStore(locationStore)
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);

          const controlMesh = builder.addRouteMesh(AutomaticTurnDispatchNames.controlChannel)
            .listen(options.controlRouterEndpoint)
            .routingId(options.rid);
          controlMesh.channel(AutomaticTurnDispatchNames.controlChannel).server();
          for (const endpoint of options.playControlEndpoints) controlMesh.peerConnections().connect(endpoint);
          const routeMesh = builder.addRouteMesh(AutomaticTurnDispatchNames.spotRouteChannel)
            .listen(options.spotRouteEndpoint)
            .routingId(options.rid);
          routeMesh.channel(AutomaticTurnDispatchNames.spotRouteChannel).server();
          for (const endpoint of options.playSpotRouteEndpoints) routeMesh.peerConnections().connect(endpoint);
          const spotMesh = builder.addRouteMesh(AutomaticTurnDispatchNames.spotChannel)
            .routingId(options.rid)
            .listen(options.spotRouterEndpoint);
          spotMesh.objects().server().addEntrySpot(AwaitSessionEntrySpot);
          spotMesh.channel(AutomaticTurnDispatchNames.spotChannel).server();
          for (const peer of options.spotRouterPeers) spotMesh.peerConnections().connect(peer.rid, peer.endpoint);
          builder.addStreamNode(AutomaticTurnDispatchNames.streamNode)
            .bind(options.streamEndpoint)
            .enableActorDispatch()
            .registerSession(AwaitSessionFactory);

          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [SESSION_OPTIONS],
        useFactory: (options: SessionOptions) => new EvidenceStore(options.rid, options.evidenceFile)
      },
      AwaitSessionEntrySpot,
      AwaitSessionFactory
    ]
  })(SessionModule);

  const app = await NestFactory.createApplicationContext(SessionModule, { logger: false, abortOnError: false });
  const options = app.get<SessionOptions>(SESSION_OPTIONS);
  const evidence = app.get(EvidenceStore);
  const frameworkRuntime = app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME, { strict: false });
  let shutdownOperation: Promise<unknown> | undefined;
  let shutdownError: unknown;
  const server = await startHttpServer(options.httpUrl, [
    {
      method: 'GET',
      path: '/health',
      handle: () => ({
        status: frameworkRuntime.status.acceptingWork ? 'ready' : 'draining',
        role: 'session',
        rid: options.rid
      })
    },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'GET',
      path: '/status',
      handle: () => ({
        state: frameworkRuntime.status.state,
        acceptingWork: frameworkRuntime.status.acceptingWork,
        sequence: frameworkRuntime.status.sequence.toString()
      })
    },
    {
      method: 'POST',
      path: '/shutdown',
      handle: () => {
        if (shutdownOperation === undefined) {
          shutdownOperation = frameworkRuntime.shutdown({ deadlineMs: 30_000 });
          void shutdownOperation.then(
            () => { stopping = true; },
            (error: unknown) => { shutdownError = error; stopping = true; }
          );
        }
        return {
          status: 'draining',
          acceptingWork: frameworkRuntime.status.acceptingWork,
          state: frameworkRuntime.status.state
        };
      }
    }
  ]);
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  if (shutdownError !== undefined) throw shutdownError;
  await app.close();
}
