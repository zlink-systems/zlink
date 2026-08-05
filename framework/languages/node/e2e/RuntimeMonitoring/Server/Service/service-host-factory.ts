import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  type ZLinkActorManager,
  type ZLinkFrameworkRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteMeshRuntime,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkSpotManager,
  type ZLinkSpotPublisherClient
} from '@zlink-systems/framework';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_PUBLISHER_CLIENT,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { PacketNames, RuntimeMonitoringNames } from '../../Shared/messages';
import { validateServiceOptions } from './Configuration/service-options';
import type { ServiceOptions, ServiceRoleOptions } from './Configuration/service-options';
import { MONITORING_OPTIONS, createMonitoringConfigurationModule } from '../../configuration';
import { createServiceEndpoints } from './Endpoints/service-endpoints';
import {
  FailingTimerHandler,
  MonitoringActor,
  MonitoringActorFactory,
  MonitoringEntryPublishHandler,
  MonitoringEntrySpot,
  MonitoringPublishGate,
  MonitoringUserSpot,
  MonitoringUserSpotPublishHandler,
  ProfileRequestHandler
} from './Handlers/service-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { PublicObserverProbe, startPublicStatusObservers } from './Support/public-status-observer';
import { createRedisLocationStore, monitoringLocationOptions } from '../../Shared/location-store';

export async function startServiceHost(role: ServiceRoleOptions = {}): Promise<void> {
  let stopping = false;

  const ServiceModule = createServiceModule(role);
  const app = await NestFactory.createApplicationContext(ServiceModule, { logger: false, abortOnError: false });
  const options = app.get(MONITORING_OPTIONS, { strict: false }) as ServiceOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const publisher = app.get(ZLINK_SPOT_PUBLISHER_CLIENT, { strict: false }) as ZLinkSpotPublisherClient;
  const spots = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as ZLinkSpotManager;
  const actors = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as ZLinkActorManager;
  const publishGate = app.get(MonitoringPublishGate, { strict: false }) as MonitoringPublishGate;
  const runtimeOptions = app.get(ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false }) as ZLinkRouteMeshRuntimeOptions;
  const routeRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const observers = startPublicStatusObservers(
    frameworkRuntime,
    routeRuntime,
    RuntimeMonitoringNames.channel,
    evidence
  );
  const observerProbe = new PublicObserverProbe();
  const server = await startHttpServer(
    options.httpUrl,
    createServiceEndpoints(
      evidence,
      runtimeOptions,
      routeRuntime,
      frameworkRuntime,
      locations,
      publisher,
      spots,
      actors,
      publishGate,
      observerProbe,
      () => { stopping = true; }
    )
  );

  try {
    while (!stopping) {
      await new Promise((resolve) => setTimeout(resolve, 100));
    }
  } finally {
    await closeHttpServer(server);
    await observerProbe.stop();
    await app.close();
    await observers.stop();
  }
}

function createServiceModule(role: ServiceRoleOptions): Function {
  class ServiceModule {}
  const configuration = createMonitoringConfigurationModule((value) => validateServiceOptions(value, role));

  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [MONITORING_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServiceOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder.configureInboundDispatch().applicationHwmBytes(4096n);
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);

          builder.addLocationStore(createRedisLocationStore({
            redisEndpoint: options.redisEndpoint,
            redisKeyPrefix: options.redisKeyPrefix
          }));
          Object.assign(builder.configureLocations(), monitoringLocationOptions());
          const profileChannelName = role.throwMonitor
            ? RuntimeMonitoringNames.throwChannel
            : RuntimeMonitoringNames.channel;
          const serviceMesh = builder.addRouteMesh(RuntimeMonitoringNames.channel)
            .listen(options.channelEndpoint)
            .routingId(options.rid);
          const spotMesh = builder.addRouteMesh(RuntimeMonitoringNames.spotChannel)
            .routingId(options.rid)
            .listen(options.spotRouterEndpoint)
            .setActorLimit(1)
            .setSpotLimit(2);
          spotMesh.objects().server()
            .addEntrySpot(MonitoringEntrySpot)
            .addSpotFactory(MonitoringUserSpot.name, MonitoringUserSpot, (factory) => factory.disableRelocation())
            .addActorFactory(RuntimeMonitoringNames.actorType, MonitoringActorFactory, (factory) => factory.disableRelocation());
          spotMesh.channel(RuntimeMonitoringNames.spotChannel).server();

          if (role.profileServer !== false) {
            serviceMesh.channel(profileChannelName).server()
              .addRequestHandler(PacketNames.profileReq, ProfileRequestHandler);
          } else {
            serviceMesh.channel(profileChannelName).client();
          }

          return {
            ...builder.build(),
          };
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [MONITORING_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ServiceOptions;
        const evidence = new EvidenceStore(options.rid, options.evidenceFile);
        MonitoringUserSpot.useEvidence(evidence);
        return evidence;
      } },
      MonitoringPublishGate,
      FailingTimerHandler,
      MonitoringActorFactory,
      MonitoringEntryPublishHandler,
      MonitoringEntrySpot,
      MonitoringUserSpot,
      MonitoringUserSpotPublishHandler,
      ProfileRequestHandler
    ]
  })(ServiceModule);
  return ServiceModule;
}
