import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  type ZLinkClientServerRuntime,
  type ZLinkChannelClient,
  type ZLinkFanoutRuntime,
  type ZLinkLocationRuntimeQuery,
  type ZLinkRouteClient,
  type ZLinkRouteMeshRuntime
} from '@zlink-systems/framework';
import {
  ZLINK_CLIENT_SERVER_RUNTIME,
  ZLINK_CHANNEL_CLIENT,
  ZLINK_FANOUT_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_OUTBOUND,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
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
import { EvidenceStore } from '../../evidence-store';
import { ConsumerFanoutEventHandler } from './Handlers/fanout-event-handler';

export async function startConsumerHost(): Promise<void> {
  let stopping = false;

  const consumer = createConsumerModule();
  const app = await NestFactory.createApplicationContext(consumer.moduleType, { logger: false, abortOnError: false });
  const options = app.get(DISCOVERY_OPTIONS, { strict: false }) as ConsumerOptions;
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const channelClient = app.get(ZLINK_CHANNEL_CLIENT, { strict: false }) as ZLinkChannelClient;
  const spotOutbound = app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as import('@zlink-systems/framework').ZLinkSpotOutbound;
  const locationQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const routeRuntime = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const clientServerRuntime = app.get(ZLINK_CLIENT_SERVER_RUNTIME, { strict: false }) as ZLinkClientServerRuntime;
  const fanoutRuntime = app.get(ZLINK_FANOUT_RUNTIME, { strict: false }) as ZLinkFanoutRuntime;
  const evidence = app.get(EvidenceStore, { strict: false }) as EvidenceStore;
  const server = await startHttpServer(
    options.httpUrl,
    createConsumerEndpoints(
      channel,
      channelClient,
      spotOutbound,
      locationQuery,
      routeRuntime,
      clientServerRuntime,
      fanoutRuntime,
      evidence,
      consumer.responseGate(),
      () => { stopping = true; }
    )
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
              .messageFlow('normal');
          responseGate = options.storeResponseGate ? createStoreResponseGate() : undefined;
          builder.addLocationStore(createGatedRedisLocationStore(options, responseGate ?? createStoreResponseGate()));
          configureStoreFailureLocationOptions(builder.configureLocations());
          const profile = builder.addRouteMesh(ChannelNames.profile);
          profile.peerConnections();
          profile.channel(ChannelNames.profile).client();
          profile.objects().client();
          if (options.multiRole) {
            const secondary = builder.addRouteMesh(ChannelNames.secondary);
            secondary.channel(ChannelNames.secondary).client();
            builder.addClientServerChannel(ChannelNames.clientServer).client();
            builder.addFanoutChannel(ChannelNames.fanout)
              .enableSubscriber()
              .addPublishHandler('FanoutEvent', ConsumerFanoutEventHandler);
          }
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [DISCOVERY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ConsumerOptions;
          return new EvidenceStore('consumer', options.evidenceFile);
        }
      },
      ConsumerFanoutEventHandler
    ]
  })(ConsumerModule);
  return { moduleType: ConsumerModule, responseGate: () => responseGate };
}
