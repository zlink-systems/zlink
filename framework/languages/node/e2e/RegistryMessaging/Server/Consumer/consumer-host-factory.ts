import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type { ZLinkLocationRuntimeQuery, ZLinkRouteClient, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { validateConsumerOptions } from './Configuration/consumer-options';
import type { ConsumerOptions } from './Configuration/consumer-options';
import { REGISTRY_MESSAGING_OPTIONS, createRegistryMessagingConfigurationModule } from '../../configuration';
import { createConsumerEndpoints } from './Endpoints/consumer-endpoints';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startConsumerHost(): Promise<void> {
  let stopping = false;
  const ConsumerModule = createConsumerModule();
  const app = await NestFactory.createApplicationContext(ConsumerModule, { logger: false, abortOnError: false });
  const options = app.get(REGISTRY_MESSAGING_OPTIONS, { strict: false }) as ConsumerOptions;
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const locationQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery | null;
  const routeRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(
    options.httpUrl,
    createConsumerEndpoints(channel, locationQuery ?? undefined, routeRuntime, () => { stopping = true; })
  );

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createConsumerModule(): Function {
  class ConsumerModule {}
  const configuration = createRegistryMessagingConfigurationModule(validateConsumerOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [REGISTRY_MESSAGING_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ConsumerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.traceLabel}-flow.log`)
              .traceLabel(options.traceLabel);

          const profile = builder.addRouteMesh('profile');
          profile.channel('profile').client();
          if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
            builder.addLocationStore(createRedisLocationStore({
              redisEndpoint: options.redisEndpoint,
              redisKeyPrefix: options.redisKeyPrefix
            }));
            locationMessagingOptions(builder.configureLocations());
            profile.peerConnections();
          } else {
            for (const endpoint of options.providerEndpoints) profile.peerConnections().connect(endpoint);
          }
          return builder.build();
        }
      })
    ]
  })(ConsumerModule);
  return ConsumerModule;
}
