import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode, type ZLinkFanoutClient } from '@zlink-systems/framework';
import { ZLINK_FANOUT_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { PubSubNames } from '../../Shared/messages';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { validatePublisherOptions, type PublisherOptions } from './Configuration/publisher-options';
import { PUBSUB_OPTIONS, createPubSubConfigurationModule } from '../../configuration';
import { createPublisherEndpoints } from './Endpoints/publisher-endpoints';
import { EvidenceDispatchErrorObserver } from './Handlers/evidence-dispatch-error-observer';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startPublisherHost(): Promise<void> {
  let stopping = false;

  class PublisherModule {}
  const configuration = createPubSubConfigurationModule(validatePublisherOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [PUBSUB_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as PublisherOptions;
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
              redisKeyPrefix: `${options.redisKeyPrefix}:location`
            }));
            locationMessagingOptions(builder.configureLocations());
          }
          const channelName = options.channelName ?? PubSubNames.channel;
          const publisher = builder.addFanoutChannel(channelName)
            .enablePublisher(options.publisherEndpoint);
          if (options.publisherAdvertiseHost !== undefined) {
            publisher.setAdvertiseHost(options.publisherAdvertiseHost);
          }
          switch (options.publisherIdentityMode ?? 'fixed') {
            case 'automatic':
              publisher.setRoutingIdPrefix(`e2e-${options.rid}`);
              break;
            case 'missing':
              break;
            case 'both':
              publisher.routingId(options.rid).setRoutingIdPrefix(`e2e-${options.rid}`);
              break;
            case 'fixed':
              publisher.routingId(options.rid);
              break;
          }
          return builder.build();
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [PUBSUB_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as PublisherOptions;
          return new EvidenceStore(options.rid, options.evidenceFile);
        }
      },
      EvidenceDispatchErrorObserver
    ]
  })(PublisherModule);

  const app = await NestFactory.createApplicationContext(PublisherModule, { logger: false, abortOnError: false });
  const options = app.get(PUBSUB_OPTIONS, { strict: false }) as PublisherOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const fanout = app.get(ZLINK_FANOUT_CLIENT, { strict: false }) as ZLinkFanoutClient;
  const server = await startHttpServer(
    options.httpUrl,
    createPublisherEndpoints(
      fanout,
      evidence,
      () => { stopping = true; },
      options.channelName ?? PubSubNames.channel
    )
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}
