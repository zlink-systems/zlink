import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode, type ZLinkLocationRuntimeQuery, type ZLinkRouteClient, type ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_LOCATION_RUNTIME_QUERY, ZLINK_ROUTE_CLIENT, ZLINK_ROUTE_MESH_RUNTIME, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { ChannelNames } from '../../Shared/messages';
import {
  configureStoreFailureLocationOptions,
  createGatedRedisLocationStore,
  createStoreResponseGate,
  type StoreResponseGate
} from '../../Shared/location-store';
import { validateConsumerOptions } from './Configuration/consumer-options';
import type { ConsumerOptions } from './Configuration/consumer-options';
import { DISCOVERY_OPTIONS, createDiscoveryConfigurationModule } from '../../configuration';
import { createConsumerEndpoints } from './Endpoints/consumer-endpoints';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startConsumerHost(): Promise<void> {
  let stopping = false;

  const consumer = createConsumerModule();
  const app = await NestFactory.createApplicationContext(consumer.moduleType, { logger: false, abortOnError: false });
  const options = app.get(DISCOVERY_OPTIONS, { strict: false }) as ConsumerOptions;
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const locationQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const routeRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(
    options.httpUrl,
    createConsumerEndpoints(channel, locationQuery, routeRuntime, consumer.responseGate(), () => { stopping = true; })
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createConsumerModule(): {
  readonly moduleType: Function;
  readonly responseGate: () => StoreResponseGate | undefined;
} {
  let responseGate: StoreResponseGate | undefined;
  class ConsumerModule {}
  const configuration = createDiscoveryConfigurationModule(validateConsumerOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [DISCOVERY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ConsumerOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.traceLabel}-flow.log`)
              .traceLabel(options.traceLabel);
          responseGate = options.storeResponseGate ? createStoreResponseGate() : undefined;
          builder.addLocationStore(createGatedRedisLocationStore(options, responseGate ?? createStoreResponseGate()));
          configureStoreFailureLocationOptions(builder.configureLocations());
          const profile = builder.addRouteMesh(ChannelNames.profile);
          profile.peerConnections();
          profile.channel(ChannelNames.profile).client();
          return builder.build();
        }
      })
    ]
  })(ConsumerModule);
  return { moduleType: ConsumerModule, responseGate: () => responseGate };
}
