import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  type ZLinkActorClient,
  type ZLinkActorManager,
  type ZLinkLocationRuntimeQuery,
  type ZLinkSpotManager,
  type ZLinkSpotOutbound
} from '@zlink-systems/framework';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import {
  ZLINK_ACTOR_CLIENT,
  ZLINK_ACTOR_MANAGER,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_SPOT_MANAGER,
  ZLINK_SPOT_OUTBOUND,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { SpotServiceNames } from '../../Shared/messages';
import { createSpotServiceConfigurationModule } from '../../configuration';
import { validateMultiNodeOptions } from './Configuration/multi-node-options';
import type { MultiNodeOptions } from './Configuration/multi-node-options';
import { createMultiNodeEndpoints } from './Endpoints/multi-node-endpoints';
import { EvidenceStore } from './Infrastructure/evidence-store';
import {
  MultiNodeCreateSpotAHandler,
  MultiNodeCreateSpotBHandler,
  MultiNodeEntryActorPingHandler,
  MultiNodeEntrySpot,
  MultiNodeScenarioActorRelocationAdapter,
  MultiNodeScenarioActorFactory,
  ScaleOutActorProbeHandler,
  MultiNodeSpotOnlyJoinHandler,
  MultiNodeSpotA,
  MultiNodeSpotB,
  MultiNodeStateAHandler,
  MultiNodeStateBHandler,
  SpotOnlyStateMsgHandler,
  SpotOnlyStateReqHandler,
  SpotOnlyUserSpot
} from './Spots/multi-node-spots';
import { closeHttpServer, startHttpServer } from './Support/http-server';

const MULTI_NODE_OPTIONS = Symbol.for('SPOT_SERVICE_MULTI_NODE_OPTIONS');

export async function startMultiNodeHost(): Promise<void> {
  const configuration = createSpotServiceConfigurationModule(MULTI_NODE_OPTIONS, validateMultiNodeOptions);
  const createEvidence = (options: MultiNodeOptions): EvidenceStore => {
    fs.mkdirSync(options.logDir, { recursive: true });
    const evidence = new EvidenceStore(options.rid, options.evidenceFile);
    MultiNodeSpotA.useEvidence(evidence);
    MultiNodeSpotB.useEvidence(evidence);
    MultiNodeEntrySpot.useEvidence(evidence);
    return evidence;
  };
  let stopping = false;

  class MultiNodeModule {}
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [MULTI_NODE_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as MultiNodeOptions;
          const isNodeA = options.rid === SpotServiceNames.multiSpotNodeA;
          const routeChannel = isNodeA ? SpotServiceNames.multiRouteChannelA : SpotServiceNames.multiRouteChannelB;
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          if (options.redisEndpoint !== undefined && options.redisKeyPrefix !== undefined) {
            builder.addLocationStore(new ZLinkRedisLocationStore({
              url: `redis://${options.redisEndpoint}`,
              keyPrefix: options.redisKeyPrefix
            }));
            builder.addRelocationStore(new ZLinkRedisRelocationStore({
              url: `redis://${options.redisEndpoint}`,
              keyPrefix: `${options.redisKeyPrefix}:relocation`
            }));
            builder.configureLocations()
              .pollingIntervalMs(100)
              .ownerLeaseRenewIntervalMs(1000)
              .ownerLeaseTtlMs(5000);
          } else {
            throw new Error('MultiNode SpotService requires the Redis location store configuration.');
          }
          const route = options.spotOnly
            ? undefined
            : builder.addRouteMesh(routeChannel)
              .listen(options.routeEndpoint)
              .routingId(options.rid);
          route?.channel(routeChannel).server();
          const spot = builder.addRouteMesh(options.spotOnly ? SpotServiceNames.spotOnlyMesh : options.rid)
            .routingId(options.rid)
            .listen(options.spotRouterEndpoint);
          const objects = spot.objects().server();
          objects.addEntrySpot(MultiNodeEntrySpot);
          objects.addActorFactory(
            options.spotOnly
              ? SpotServiceNames.alternateActorType
              : SpotServiceNames.actorType,
            MultiNodeScenarioActorFactory,
            (factory) => factory.preserveStateWith(
              MultiNodeScenarioActorRelocationAdapter
            )
          );
          objects.addSpotFactory(
            SpotOnlyUserSpot.name,
            SpotOnlyUserSpot,
            (factory) => factory.disableRelocation()
          );
          spot.channel(options.spotOnly ? SpotServiceNames.spotOnlyMesh : options.rid).server();
          if (options.peerSpotRouterEndpoint !== undefined) {
            spot.peerConnections().connect(
              isNodeA ? SpotServiceNames.multiSpotNodeB : SpotServiceNames.multiSpotNodeA,
              options.peerSpotRouterEndpoint
            );
          }
          if (isNodeA) {
            route?.addRequestHandler('MultiNodeCreateSpotReq', MultiNodeCreateSpotAHandler);
            objects.addSpotFactory(
              MultiNodeSpotA.name,
              MultiNodeSpotA,
              (factory) => factory.disableRelocation()
            );
          } else {
            route?.addRequestHandler('MultiNodeCreateSpotReq', MultiNodeCreateSpotBHandler);
            objects.addSpotFactory(
              MultiNodeSpotB.name,
              MultiNodeSpotB,
              (factory) => factory.disableRelocation()
            );
          }
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [MULTI_NODE_OPTIONS], useFactory: createEvidence },
      MultiNodeCreateSpotAHandler,
      MultiNodeCreateSpotBHandler,
      MultiNodeEntryActorPingHandler,
      MultiNodeEntrySpot,
      MultiNodeScenarioActorRelocationAdapter,
      MultiNodeScenarioActorFactory,
      ScaleOutActorProbeHandler,
      MultiNodeSpotOnlyJoinHandler,
      MultiNodeStateAHandler,
      MultiNodeStateBHandler,
      SpotOnlyStateMsgHandler,
      SpotOnlyStateReqHandler
    ]
  })(MultiNodeModule);

  const app = await NestFactory.createApplicationContext(MultiNodeModule, { logger: false, abortOnError: false });
  const options = app.get(MULTI_NODE_OPTIONS) as MultiNodeOptions;
  const evidence = app.get(EvidenceStore);
  const spots = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as ZLinkSpotManager;
  const outbound = app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as ZLinkSpotOutbound;
  const actors = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as ZLinkActorManager;
  const actorClient = app.get(ZLINK_ACTOR_CLIENT, { strict: false }) as ZLinkActorClient;
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  const runtimeOptions = app.get(
    ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
    { strict: false }
  ) as import('@zlink-systems/framework').ZLinkRouteMeshRuntimeOptions;
  SpotOnlyUserSpot.configureDependencies(evidence, spots);
  const server = await startHttpServer(options.httpUrl, createMultiNodeEndpoints(
    evidence,
    spots,
    outbound,
    spots,
    actors,
    actorClient,
    locations,
    runtimeOptions,
    options.spotOnly ? SpotServiceNames.spotOnlyMesh : options.rid,
    () => { stopping = true; }
  ));
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}
