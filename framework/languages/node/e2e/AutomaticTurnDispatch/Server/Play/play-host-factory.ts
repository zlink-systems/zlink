import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode, ZLinkUserSpotExecutionMode, type ZLinkFrameworkRuntime } from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { ZLinkHttpClientModule, ZLinkModule, ZLINK_FRAMEWORK_RUNTIME, zlinkFramework } from '@zlink-systems/nestjs';
import { AutomaticTurnDispatchNames } from '../../Shared/messages';
import { EvidenceStore } from './Support/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { createAutomaticTurnConfiguration } from '../../configuration';
import { PLAY_OPTIONS, validatePlayOptions } from './Configuration/play-options';
import type { PlayOptions } from './Configuration/play-options';
import { HoldCommandHandler, ProbeCommandHandler, ProbeRequestHandler, WorkerAwaitCommandHandler, AwaitCommandHandler, AwaitRequestHandler } from './Handlers/basic-spot-handlers';
import {
  CounterAwaitHandler,
  CounterReadHandler,
  CounterResetHandler,
  CpuWorkerAwaitHandler,
  HttpAwaitHandler,
  IoWorkerBatchHandler,
  SelfCycleHandler,
  SelfSendHandler
} from './Handlers/execution-turn-handlers';
import {
  BindAwaitActorsControlHandler,
  EnsureSpotControlHandler,
  YIELD_PLAY_NODE_RID,
  AwaitEvidenceControlHandler,
  AwaitEvidenceWaitControlHandler
} from './Handlers/control-handlers';
import { AwaitCancelCommandHandler, AwaitTimeoutCommandHandler } from './Handlers/failure-spot-handlers';
import { RemoteSpotAwaitCommandHandler, RemoteSpotAwaitHandler } from './Handlers/remote-spot-handlers';
import { TimerStartCommandHandler, TimerStopCommandHandler, AwaitTimerHandler } from './Handlers/timer-spot-handlers';
import {
  EntryActorFastHandler,
  EntryActorFastSendHandler,
  EntryActorJoinAwaitHandler,
  EntryActorPushAwaitHandler,
  EntryActorAwaitHandler,
  SpotActorFastHandler,
  SpotActorFastSendHandler,
  SpotActorDeferredJoinFailureHandler,
  SpotActorJoinAwaitHandler,
  SpotActorPushAwaitHandler,
  SpotActorAwaitHandler,
  AwaitActorFactory,
  AwaitEntrySpot
} from './Spots/await-actors';
import { AwaitProbeSpot } from './Spots/await-probe-spot';
import { PerActorAwaitProbeSpot } from './Spots/per-actor-await-probe-spot';

export async function startPlayHost(): Promise<void> {
  let stopping = false;
  const configured = createAutomaticTurnConfiguration(PLAY_OPTIONS, validatePlayOptions);
  const configuration = configured.module;
  const frameworkModule = ZLinkModule.forRootFactory({
    imports: [configuration],
    inject: [PLAY_OPTIONS],
    useFactory: (value: unknown) => {
      const options = value as PlayOptions;
      fs.mkdirSync(options.logDir, { recursive: true });
      const locationStore = new ZLinkRedisLocationStore({
        url: `redis://${options.redisEndpoint}`,
        keyPrefix: options.redisKeyPrefix
      });
      const builder = zlinkFramework();
      builder
        .configureDispatch()
          .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
          .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
          .traceLabel(options.rid);
      builder.addLocationStore(locationStore);
      builder.addRouteMesh(AutomaticTurnDispatchNames.controlChannel)
        .listen(options.controlEndpoint)
        .routingId(options.rid)
        .addRequestHandler('EnsureSpotReq', EnsureSpotControlHandler)
        .addRequestHandler('BindAwaitActorsReq', BindAwaitActorsControlHandler)
        .addRequestHandler('AwaitEvidenceReq', AwaitEvidenceControlHandler)
        .addRequestHandler('AwaitEvidenceWaitReq', AwaitEvidenceWaitControlHandler)
        .channel(AutomaticTurnDispatchNames.controlChannel).server();
      const delayMesh = builder.addRouteMesh(AutomaticTurnDispatchNames.delayChannel);
      delayMesh.peerConnections().connect(options.delayEndpoint);
      delayMesh.channel(AutomaticTurnDispatchNames.delayChannel).client();
      const spotRoute = builder.addRouteMesh(AutomaticTurnDispatchNames.spotRouteChannel)
        .listen(options.spotRouteEndpoint)
        .routingId(options.rid);
      spotRoute.channel(AutomaticTurnDispatchNames.spotRouteChannel).server();
      for (const endpoint of options.peerSpotRouteEndpoints) {
        spotRoute.peerConnections().connect(endpoint);
      }
      const spotMesh = builder.addRouteMesh(AutomaticTurnDispatchNames.spotChannel)
        .routingId(options.rid)
        .listen(options.spotRouterEndpoint);
      const objectServer = spotMesh.objects().server();
      objectServer.addEntrySpot(AwaitEntrySpot);
      objectServer.addActorFactory(
        AutomaticTurnDispatchNames.actorType,
        AwaitActorFactory,
        (factory) => factory.disableRelocation()
      );
      objectServer.addSpotFactory(
        AwaitProbeSpot.name,
        AwaitProbeSpot,
        (factory) => factory.disableRelocation()
      );
      objectServer.addSpotFactory(
        AutomaticTurnDispatchNames.perActorSpotType,
        PerActorAwaitProbeSpot,
        (factory) => {
          factory.executionMode(ZLinkUserSpotExecutionMode.PerActor);
          factory.recreateOnRelocation();
        }
      );
      spotMesh.channel(AutomaticTurnDispatchNames.spotChannel).server();
      for (const peer of options.spotRouterPeers) spotMesh.peerConnections().connect(peer.rid, peer.endpoint);
      return builder.build();
    }
  });
  const httpClientModule = ZLinkHttpClientModule.forRoot({
    imports: [frameworkModule],
    clients: [{ name: 'external-api', baseUrl: configured.options.externalApiUrl }]
  });

  class PlayModule {}
  Module({
    imports: [
      configuration,
      frameworkModule,
      httpClientModule
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [PLAY_OPTIONS],
        useFactory: (options: PlayOptions) => new EvidenceStore(options.rid, options.evidenceFile)
      },
      {
        provide: YIELD_PLAY_NODE_RID,
        inject: [PLAY_OPTIONS],
        useFactory: (options: PlayOptions) => options.rid
      },
      EnsureSpotControlHandler,
      BindAwaitActorsControlHandler,
      AwaitEvidenceControlHandler,
      AwaitEvidenceWaitControlHandler,
      HoldCommandHandler,
      AwaitCommandHandler,
      AwaitRequestHandler,
      ProbeRequestHandler,
      CounterResetHandler,
      CounterAwaitHandler,
      CounterReadHandler,
      HttpAwaitHandler,
      IoWorkerBatchHandler,
      CpuWorkerAwaitHandler,
      SelfCycleHandler,
      SelfSendHandler,
      WorkerAwaitCommandHandler,
      AwaitTimeoutCommandHandler,
      AwaitCancelCommandHandler,
      ProbeCommandHandler,
      RemoteSpotAwaitHandler,
      RemoteSpotAwaitCommandHandler,
      TimerStartCommandHandler,
      TimerStopCommandHandler,
      AwaitTimerHandler,
      AwaitActorFactory,
      AwaitEntrySpot,
      EntryActorAwaitHandler,
      EntryActorFastHandler,
      EntryActorFastSendHandler,
      EntryActorJoinAwaitHandler,
      EntryActorPushAwaitHandler,
      SpotActorAwaitHandler,
      SpotActorFastHandler,
      SpotActorFastSendHandler,
      SpotActorDeferredJoinFailureHandler,
      SpotActorJoinAwaitHandler,
      SpotActorPushAwaitHandler,
      AwaitProbeSpot,
      PerActorAwaitProbeSpot
    ]
  })(PlayModule);

  const app = await NestFactory.createApplicationContext(PlayModule, { logger: false, abortOnError: false });
  const options = app.get<PlayOptions>(PLAY_OPTIONS);
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
        role: 'play',
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
