import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { ZLinkMessageFlowLogMode } from '@zlink-systems/framework';
import { ZLINK_LOCATION_RUNTIME_QUERY, ZLINK_ROUTE_CLIENT, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import type { ZLinkLocationRuntimeQuery, ZLinkRouteClient } from '@zlink-systems/framework';
import type { ProfileRes, ProfileReq } from '../../Shared/messages';
import { PacketNames, ResilienceNames } from '../../Shared/messages';
import { validateConsumerOptions } from './Configuration/consumer-options';
import type { ConsumerOptions } from './Configuration/consumer-options';
import { RESILIENCE_OPTIONS, createResilienceConfigurationModule } from '../../configuration';
import { createConsumerEndpoints, requestProfile } from './Endpoints/consumer-endpoints';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { createRedisLocationStore, resilienceLocationOptions } from '../../Shared/location-store';
import { LoadEventHandler } from './Handlers/load-event-handler';
import { EvidenceStore } from './Infrastructure/evidence-store';

export async function startConsumerHost(): Promise<void> {
  let stopping = false;
  const ConsumerModule = createConfiguredConsumerModule();
  const app = await NestFactory.createApplicationContext(ConsumerModule, { logger: false, abortOnError: false });
  const options = app.get(RESILIENCE_OPTIONS, { strict: false }) as ConsumerOptions;
  const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const locationQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const evidence = app.get(EvidenceStore, { strict: false });
  const server = await startHttpServer(
    options.httpUrl,
    createConsumerEndpoints(channel, locationQuery, evidence, (request) => requestWithNewClient(options, request), () => { stopping = true; })
  );

  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createConfiguredConsumerModule(): Function {
  class ConsumerModule {}
  const configuration = createResilienceConfigurationModule(validateConsumerOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [RESILIENCE_OPTIONS],
        useFactory: (value: unknown) => buildFramework(value as ConsumerOptions, undefined, true)
      })
    ],
    providers: [
      {
        provide: EvidenceStore,
        inject: [RESILIENCE_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ConsumerOptions;
          return new EvidenceStore(options.rid, options.evidenceFile);
        }
      },
      LoadEventHandler
    ]
  })(ConsumerModule);
  return ConsumerModule;
}

async function requestWithNewClient(options: ConsumerOptions, request: ProfileReq): Promise<ProfileRes> {
  const traceLabel = `storm-${request.marker ?? 'request'}`;
  const ConsumerModule = createConsumerModule(options, traceLabel);
  const app = await NestFactory.createApplicationContext(ConsumerModule, { logger: false, abortOnError: false });
  try {
    const channel = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
    return await requestProfile(channel, request, 5000);
  } finally {
    // Each storm iteration owns a complete runtime. Do not start the next one
    // until its sockets, location leases, and background tasks have stopped.
    await app.close();
  }
}

function createConsumerModule(options: ConsumerOptions, traceLabel = options.traceLabel): Function {
  class ConsumerModule {}
  Module({
    imports: [
      ZLinkModule.forRootFactory({
        useFactory: () => buildFramework(options, traceLabel)
      })
    ]
  })(ConsumerModule);
  return ConsumerModule;
}

function buildFramework(options: ConsumerOptions, traceLabel = options.traceLabel, includeFanout = false) {
  fs.mkdirSync(options.logDir, { recursive: true });
  const builder = zlinkFramework();
  builder.configureDispatch()
    .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
    .traceLogFile(`${options.logDir}/${traceLabel}-flow.log`)
    .traceLabel(traceLabel);
  const profile = builder.addRouteMesh('profile')
    .listen('tcp://127.0.0.1:0')
    .routingId(options.rid);
  profile.channel('profile').client();
  if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
    builder.addLocationStore(createRedisLocationStore({ redisEndpoint: options.redisEndpoint, redisKeyPrefix: options.redisKeyPrefix }));
    Object.assign(builder.configureLocations(), resilienceLocationOptions());
    profile.peerConnections();
    if (includeFanout) {
      builder.addFanoutChannel(ResilienceNames.fanoutChannel)
        .enableSubscriber()
        .addPublishHandler(PacketNames.loadEvent, LoadEventHandler);
    }
  } else {
    for (const endpoint of options.providerEndpoints) profile.peerConnections().connect(endpoint);
  }
  return builder.build();
}
