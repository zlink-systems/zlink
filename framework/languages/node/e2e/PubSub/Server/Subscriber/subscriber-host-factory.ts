import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode, type ZLinkFanoutRuntime, type ZLinkLocationRuntimeQuery } from '@zlink-systems/framework';
import {
  ZLINK_FANOUT_RUNTIME,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { PacketNames, PubSubNames } from '../../Shared/messages';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import { validateSubscriberOptions, SUBSCRIBER_OPTIONS, type SubscriberOptions } from './Configuration/subscriber-options';
import { PUBSUB_OPTIONS, createPubSubConfigurationModule } from '../../configuration';
import { createSubscriberEndpoints } from './Endpoints/operational-endpoints';
import { EvidenceDispatchErrorObserver, EventMsgHandler } from './Handlers/event-msg-handler';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { FanoutStatusObserverProbe } from './Support/fanout-status-observer';

export async function startSubscriberHost(): Promise<void> {
  let stopping = false;

  const SubscriberModule = createSubscriberModule();
  const app = await NestFactory.createApplicationContext(SubscriberModule, { logger: false, abortOnError: false });
  const options = app.get(PUBSUB_OPTIONS, { strict: false }) as SubscriberOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const fanoutRuntime = app.get(ZLINK_FANOUT_RUNTIME, { strict: false }) as ZLinkFanoutRuntime | undefined;
  const locationRuntimeQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as
    ZLinkLocationRuntimeQuery | undefined;
  const observerProbe = new FanoutStatusObserverProbe(
    fanoutRuntime,
    evidence,
    options.channelName ?? PubSubNames.channel
  );
  const server = await startHttpServer(
    options.httpUrl,
    createSubscriberEndpoints(
      evidence,
      fanoutRuntime,
      () => { stopping = true; },
      observerProbe,
      locationRuntimeQuery,
      options.channelName ?? PubSubNames.channel
    )
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await observerProbe.stop();
  await closeHttpServer(server);
  await app.close();
}

function createSubscriberModule(): Function {
  class SubscriberModule {}
  const configuration = createPubSubConfigurationModule(validateSubscriberOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [PUBSUB_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as SubscriberOptions;
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
          const fanout = builder.addFanoutChannel(options.channelName ?? PubSubNames.channel)
            .enableSubscriber(options.publisherEndpoint)
            .routingId(options.rid)
            .addPublishHandler(PacketNames.eventMsg, EventMsgHandler);
          if (options.subscriberMode === 'mixed') {
            fanout.enableSubscriber();
          }
          return {
            ...builder.build(),
          };
        }
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [PUBSUB_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as SubscriberOptions;
          return new EvidenceStore(options.rid, options.evidenceFile);
        }
      },
      { provide: SUBSCRIBER_OPTIONS, useExisting: PUBSUB_OPTIONS },
      EventMsgHandler,
      EvidenceDispatchErrorObserver
    ]
  })(SubscriberModule);
  return SubscriberModule;
}
