import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import type { ZLinkRouteMeshRuntime, ZLinkSpotPublisherClient } from '@zlink-systems/framework';
import {
  ZLinkMessageFlowLogMode
} from '@zlink-systems/framework';
import { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLINK_SPOT_PUBLISHER_CLIENT,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import type { SpotPublishReq } from '../../Shared/messages';
import { SpotMsg, SpotServiceNames, spotServicePacket } from '../../Shared/messages';
import { createSpotServiceConfigurationModule, objectValues, optionalString, requiredString } from '../../configuration';
import { EvidenceStore } from '../Play/Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer, type HttpRoute } from '../Play/Support/http-server';

interface GatewayOptions {
  readonly rid: string;
  readonly httpUrl: string;
  readonly spotRouterEndpoint: string;
  readonly redisEndpoint: string;
  readonly redisKeyPrefix: string;
  readonly evidenceFile?: string;
  readonly logDir: string;
}

const GATEWAY_OPTIONS = Symbol.for('SPOT_SERVICE_GATEWAY_OPTIONS');

export async function startGatewayHost(): Promise<void> {
  const configuration = createSpotServiceConfigurationModule(GATEWAY_OPTIONS, validateGatewayOptions);
  const createEvidence = (options: GatewayOptions): EvidenceStore => {
    fs.mkdirSync(options.logDir, { recursive: true });
    const evidence = new EvidenceStore(options.rid, options.evidenceFile);
    evidence.add(`start|rid=${options.rid}`);
    return evidence;
  };
  let stopping = false;

  class GatewayModule {}
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [GATEWAY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as GatewayOptions;
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          builder.addLocationStore(new ZLinkRedisLocationStore({
            url: `redis://${options.redisEndpoint}`,
            keyPrefix: options.redisKeyPrefix
          }));
          builder.configureLocations()
            .pollingIntervalMs(100)
            .ownerLeaseRenewIntervalMs(1000)
            .ownerLeaseTtlMs(5000);
          builder.addRouteMesh(SpotServiceNames.spotChannel)
            .routingId(options.rid)
            .listen(options.spotRouterEndpoint)
            .channel(SpotServiceNames.spotChannel)
            .client();
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [GATEWAY_OPTIONS], useFactory: createEvidence }
    ]
  })(GatewayModule);

  const app = await NestFactory.createApplicationContext(GatewayModule, { logger: false, abortOnError: false });
  const options = app.get(GATEWAY_OPTIONS) as GatewayOptions;
  const evidence = app.get(EvidenceStore);
  const publisher = app.get(ZLINK_SPOT_PUBLISHER_CLIENT, { strict: false }) as ZLinkSpotPublisherClient;
  const routeMesh = app.get(ZLINK_ROUTE_MESH_RUNTIME, { strict: false }) as ZLinkRouteMeshRuntime;
  const server = await startHttpServer(
    options.httpUrl,
    createGatewayEndpoints(options, evidence, publisher, routeMesh, () => { stopping = true; })
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createGatewayEndpoints(
  options: GatewayOptions,
  evidence: EvidenceStore,
  publisher: ZLinkSpotPublisherClient,
  routeMesh: ZLinkRouteMeshRuntime,
  stop: () => void
): HttpRoute[] {
  return [
    {
      method: 'GET',
      path: '/health',
      handle: () => {
        if (!routeMesh.isReady(SpotServiceNames.spotChannel)) {
          throw new Error('Gateway RouteMesh peer is not connected yet.');
        }
        return { status: 'ready', role: 'gateway', rid: options.rid };
      }
    },
    { method: 'GET', path: '/evidence', handle: () => evidence.snapshot() },
    {
      method: 'POST',
      path: '/spot/publish',
      handle: async (body) => {
        const request = body as SpotPublishReq;
        await publisher
          .publish(
            SpotServiceNames.spotChannel,
            SpotServiceNames.spotChannel,
            SpotServiceNames.spotEventTopic,
            spotServicePacket(SpotMsg, { marker: request.marker }))
          .submit();
        evidence.add(`spot-publish|rid=${options.rid}|spot=${request.spotId}|marker=${request.marker}`);
        return {
          operation: 'spot.sm-c4-publish',
          publisherRid: options.rid,
          spotId: request.spotId,
          marker: request.marker,
          evidence: evidence.snapshot()
        };
      }
    },
    { method: 'POST', path: '/shutdown', handle: () => { stop(); return { status: 'stopping' }; } }
  ];
}

function validateGatewayOptions(value: unknown): GatewayOptions {
  const values = objectValues(value);
  return {
    rid: requiredString(values, 'rid'),
    httpUrl: requiredString(values, 'httpUrl'),
    spotRouterEndpoint: requiredString(values, 'spotRouterEndpoint'),
    redisEndpoint: requiredString(values, 'redisEndpoint'),
    redisKeyPrefix: requiredString(values, 'redisKeyPrefix'),
    evidenceFile: optionalString(values, 'evidenceFile'),
    logDir: requiredString(values, 'logDir')
  };
}
