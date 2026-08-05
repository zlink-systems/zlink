import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import {
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_ROUTE_CLIENT,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type {
  ZLinkFrameworkRuntime,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntimeOptions
} from '@zlink-systems/framework';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { PacketNames } from '../../Shared/messages';
import { validateServerOptions } from './Configuration/server-options';
import type { ServerOptions } from './Configuration/server-options';
import { REGISTRY_MESSAGING_OPTIONS, createRegistryMessagingConfigurationModule } from '../../configuration';
import { createProviderEndpoints } from './Endpoints/provider-endpoints';
import {
  EvidenceDispatchErrorObserver,
  PayloadRequestHandler,
  ProfileCommandHandler,
  ProfileRequestHandler,
  RoutePingHandler
} from './Handlers/provider-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startProviderHost(): Promise<void> {
  let stopping = false;

  const ProviderModule = createProviderModule();
  const app = await NestFactory.createApplicationContext(ProviderModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRY_MESSAGING_OPTIONS, { strict: false }) as ServerOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const route = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const runtimeOptions = app.get(ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false }) as ZLinkRouteMeshRuntimeOptions;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  const server = await startHttpServer(
    options.httpUrl,
    createProviderEndpoints(evidence, route, route, runtimeOptions, frameworkRuntime, () => { stopping = true; })
  );

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createProviderModule(): Function {
  class ProviderModule {}
  const configuration = createRegistryMessagingConfigurationModule(validateServerOptions);

  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [REGISTRY_MESSAGING_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .setMessageFlowObserver(EvidenceDispatchErrorObserver)
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);

          if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
            builder.addLocationStore(createRedisLocationStore({
              redisEndpoint: options.redisEndpoint,
              redisKeyPrefix: options.redisKeyPrefix
            }));
            locationMessagingOptions(builder.configureLocations());
          }
          if (options.channelEndpoint !== undefined) {
            const profile = builder.addRouteMesh('profile')
              .listen(options.channelEndpoint)
              .routingId(options.rid);
            profile.peerConnections();
            profile.channel('profile').server()
              .setWeight(options.weight)
              .addRequestHandler(PacketNames.profileReq, ProfileRequestHandler)
              .addRequestHandler(PacketNames.payloadReq, PayloadRequestHandler)
              .addSendHandler(PacketNames.profileMsg, ProfileCommandHandler);
          }
          if (options.manualClientEndpoint !== undefined) {
            builder.addRouteMesh('profile.manual')
              .peerConnections().connect(options.manualClientEndpoint);
          }
          if (options.routeEndpoint !== undefined) {
            const routeMesh = builder.addRouteMesh('profile.route')
              .listen(options.routeEndpoint)
              .routingId(options.rid)
              .addRequestHandler(PacketNames.scenarioRouteReq, RoutePingHandler);
            for (const endpoint of options.routePeers) routeMesh.peerConnections().connect(endpoint);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [REGISTRY_MESSAGING_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ServerOptions; return new EvidenceStore(options.rid, options.evidenceFile);
      } },
      EvidenceDispatchErrorObserver,
      PayloadRequestHandler,
      ProfileCommandHandler,
      ProfileRequestHandler,
      RoutePingHandler
    ]
  })(ProviderModule);
  return ProviderModule;
}
