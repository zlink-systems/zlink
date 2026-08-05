import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import {
  ZLINK_FANOUT_CLIENT,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type {
  ZLinkFanoutClient,
  ZLinkRouteClient,
  ZLinkRouteMeshRuntimeOptions
} from '@zlink-systems/framework';
import { PacketNames, ResilienceNames } from '../../Shared/messages';
import { validateServerOptions } from './Configuration/server-options';
import type { ServerOptions } from './Configuration/server-options';
import { RESILIENCE_OPTIONS, createResilienceConfigurationModule } from '../../configuration';
import { createProviderEndpoints } from './Endpoints/provider-endpoints';
import {
  EvidenceDispatchErrorObserver,
  EvidenceRuntimeErrorSink,
  PayloadRequestHandler,
  ProfileCommandHandler,
  ProfileRequestHandler,
} from './Handlers/provider-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { FaultState } from './Infrastructure/fault-state';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { createRedisLocationStore, resilienceLocationOptions } from '../../Shared/location-store';

export async function startProviderHost(): Promise<void> {
  let stopping = false;

  const ProviderModule = createProviderModule();
  const app = await NestFactory.createApplicationContext(ProviderModule, { logger: false, abortOnError: false });
  const options = app.get(RESILIENCE_OPTIONS, { strict: false }) as ServerOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const fault = app.get(FaultState, { strict: false });
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const fanout = app.get(ZLINK_FANOUT_CLIENT, { strict: false }) as ZLinkFanoutClient;
  const runtimeOptions = app.get(
    ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
    { strict: false }
  ) as ZLinkRouteMeshRuntimeOptions;
  const server = await startHttpServer(options.httpUrl, createProviderEndpoints(
    evidence,
    fault,
    channel,
    fanout,
    runtimeOptions,
    () => { stopping = true; }
  ));

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createProviderModule(): Function {
  class ProviderModule {}
  const configuration = createResilienceConfigurationModule(validateServerOptions);

  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [RESILIENCE_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ServerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .setMessageFlowObserver(EvidenceDispatchErrorObserver)
              .setRuntimeErrorSink(EvidenceRuntimeErrorSink)
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);

          if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
            builder.addLocationStore(createRedisLocationStore({
              redisEndpoint: options.redisEndpoint,
              redisKeyPrefix: options.redisKeyPrefix
            }));
            Object.assign(builder.configureLocations(), resilienceLocationOptions());
          }
          if (options.channelEndpoint !== undefined) {
            const profile = builder.addRouteMesh('profile')
              .listen(options.channelEndpoint)
              .routingId(options.rid);
            profile.peerConnections();
            profile.channel('profile').server()
              .addRequestHandler(PacketNames.profileReq, ProfileRequestHandler)
              .addRequestHandler(PacketNames.payloadReq, PayloadRequestHandler)
              .addSendHandler(PacketNames.profileMsg, ProfileCommandHandler);
          }
          if (options.fanoutEndpoint !== undefined) {
            builder.addFanoutChannel(ResilienceNames.fanoutChannel)
              .enablePublisher(options.fanoutEndpoint);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [RESILIENCE_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ServerOptions; return new EvidenceStore(options.rid, options.evidenceFile);
      } },
      FaultState,
      EvidenceDispatchErrorObserver,
      EvidenceRuntimeErrorSink,
      PayloadRequestHandler,
      ProfileCommandHandler,
      ProfileRequestHandler,
    ]
  })(ProviderModule);
  return ProviderModule;
}
