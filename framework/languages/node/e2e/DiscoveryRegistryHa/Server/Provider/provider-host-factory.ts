import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  type ZLinkFanoutClient,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime
} from '@zlink-systems/framework';
import {
  ZLINK_ACTOR_MANAGER,
  ZLINK_ACTOR_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLINK_FANOUT_CLIENT,
  ZLINK_LOCATION_RUNTIME_QUERY,
  ZLINK_SPOT_MANAGER,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { ChannelNames, ObjectSpotType, PacketNames } from '../../Shared/messages';
import { configureStoreFailureLocationOptions, createRedisLocationStore } from '../../Shared/location-store';
import {
  ZLinkRedisLocationStore,
  ZLinkRedisRelocationStore
} from '@zlink-systems/framework-locations-redis';
import { validateProviderOptions } from './Configuration/provider-options';
import type { ProviderOptions } from './Configuration/provider-options';
import { DISCOVERY_OPTIONS, createDiscoveryConfigurationModule } from '../../configuration';
import { createProviderEndpoints } from './Endpoints/provider-endpoints';
import { ProfileRequestHandler } from './Handlers/profile-request-handler';
import {
  Config6InstanceSpot,
  Config6InstanceTimer,
  configureObjectEvidence,
  configureObjectActivationDelay,
  ObjectRequestHandler
} from './Handlers/object-request-handler';
import {
  Config6ActorFactory,
  Config6ActorAdapter,
  Config6ActorType,
  Config6EntrySpot,
  Config6UserSpot,
  Config6UserSpotAdapter,
  Config6LeaveHandler,
  Config6JoinHandler,
  Config6ProbeHandler,
  Config6UserSpotType
} from './Handlers/capacity-objects';
import {
  ClientServerRequestHandler,
  SecondaryRequestHandler
} from './Handlers/multi-role-handlers';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';
import { configureScenarioGates } from './Infrastructure/scenario-gates';

export async function startProviderHost(): Promise<void> {
  let stopping = false;

  const provider = createProviderModule();
  const app = await NestFactory.createApplicationContext(provider.moduleType, { logger: false, abortOnError: false });
  const options = app.get(DISCOVERY_OPTIONS, { strict: false }) as ProviderOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  configureObjectEvidence(evidence);
  configureScenarioGates(evidence);
  configureObjectActivationDelay(
    options.capacityProfile === 'sf-c5a'
      ? 3_000
      : options.capacityProfile === 'sf-g2'
        ? 40
        : 0
  );
  const runtimeOptions = app.get(ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false }) as ZLinkRouteMeshRuntimeOptions;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  const fanout = app.get(ZLINK_FANOUT_CLIENT, { strict: false }) as ZLinkFanoutClient;
  const locationQuery = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as import('@zlink-systems/framework').ZLinkLocationRuntimeQuery;
  const actorManager = app.get(ZLINK_ACTOR_MANAGER, { strict: false }) as import('@zlink-systems/framework').ZLinkActorManager;
  const actorClient = app.get(ZLINK_ACTOR_CLIENT, { strict: false }) as import('@zlink-systems/framework').ZLinkActorClient;
  const spotManager = app.get(ZLINK_SPOT_MANAGER, { strict: false }) as import('@zlink-systems/framework').ZLinkSpotManager;
  const server = await startHttpServer(
    options.httpUrl,
    createProviderEndpoints(
      evidence,
      runtimeOptions,
      frameworkRuntime,
      locationQuery,
      actorManager,
      actorClient,
      spotManager,
      fanout,
      () => { stopping = true; }
    )
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  evidence.add(`host-stopping|rid=${evidence.rid}|phase=http-close`);
  await closeHttpServer(server);
  evidence.add(`host-stopping|rid=${evidence.rid}|phase=nest-close`);
  await app.close();
  await provider.disposeLocationStore();
  evidence.add(`host-stopped|rid=${evidence.rid}`);
}

function createProviderModule(): {
  readonly moduleType: Function;
  readonly disposeLocationStore: () => Promise<void>;
} {
  let locationStore: ZLinkRedisLocationStore | undefined;
  let relocationStore: ZLinkRedisRelocationStore | undefined;
  class ProviderModule {}
  const configuration = createDiscoveryConfigurationModule(validateProviderOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [DISCOVERY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ProviderOptions;
          const unlimitedPopulation = options.capacityProfile === 'sf-g2';
          const atomicCapacity = options.capacityProfile === 'sf-g1';
          const aggregateCapacity = options.capacityProfile.startsWith('sf-g3-');
          const stateRelocation = aggregateCapacity
            || options.capacityProfile.startsWith('sf-f7-')
            || options.capacityProfile.startsWith('sf-f');
          const aggregateTarget = aggregateCapacity && options.rid === 'api-b';
          const actorLimit = aggregateCapacity
            ? aggregateTarget && options.capacityProfile === 'sf-g3-short-actor' ? 1 : 8
            : unlimitedPopulation ? 0 : atomicCapacity ? 3 : 10_000;
          const spotLimit = aggregateCapacity
            ? aggregateTarget && options.capacityProfile === 'sf-g3-short-spot' ? 1 : 8
            : unlimitedPopulation ? 0 : atomicCapacity ? 4 : 2000;
          const userSpotTypeLimit = aggregateCapacity
            ? aggregateTarget && options.capacityProfile === 'sf-g3-short-type' ? 1 : 8
            : unlimitedPopulation ? 0 : atomicCapacity ? 3 : 2000;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow('normal');
          locationStore = createRedisLocationStore(options);
          builder.addLocationStore(locationStore);
          relocationStore = new ZLinkRedisRelocationStore({
            url: `redis://${options.relocationRedisEndpoint}`,
            keyPrefix: `${options.redisKeyPrefix}:relocation`
          });
          builder.addRelocationStore(relocationStore);
          configureStoreFailureLocationOptions(builder.configureLocations());
          const profile = builder.addRouteMesh(ChannelNames.profile)
            .listen(options.channelEndpoint)
            .routingId(options.rid)
            .setActorLimit(actorLimit)
            .setSpotLimit(spotLimit)
            .setActivationConcurrency(unlimitedPopulation ? 2 : atomicCapacity ? 4 : 64);
          profile.channel(ChannelNames.profile).server()
            .addRequestHandler(PacketNames.profileReq, ProfileRequestHandler);
          if (options.multiRole) {
            const secondary = builder.addRouteMesh(ChannelNames.secondary)
              .listen(0)
              .setRoutingIdPrefix(`${options.rid}-secondary`);
            secondary.channel(ChannelNames.secondary).server()
              .addRequestHandler(PacketNames.secondaryReq, SecondaryRequestHandler);
            builder.addClientServerChannel(ChannelNames.clientServer).server()
              .listen(0)
              .addRequestHandler(PacketNames.clientServerReq, ClientServerRequestHandler);
            const fanout = builder.addFanoutChannel(ChannelNames.fanout);
            if (options.fanoutEndpoint !== undefined) fanout.enablePublisher(options.fanoutEndpoint);
            else fanout.enablePublisher(0);
            fanout.setRoutingIdPrefix(`${options.rid}-fanout`);
          }
          const objects = profile.objects().server();
          objects.addEntrySpot(Config6EntrySpot);
          objects.addActorFactory(
            Config6ActorType,
            Config6ActorFactory,
            (factory) => stateRelocation
              ? factory.preserveStateWith(Config6ActorAdapter)
              : factory.disableRelocation()
          );
          objects.addSpotFactory(
            Config6UserSpotType,
            Config6UserSpot,
            (factory) => {
              if (stateRelocation) factory.preserveStateWith(Config6UserSpotAdapter);
              else factory.disableRelocation();
              factory.stableTypeLimit(userSpotTypeLimit);
            }
          );
          objects.addInstanceSpotFactory(
            ObjectSpotType,
            Config6InstanceSpot,
            (factory) => {
              if (stateRelocation) factory.recreateOnRelocation();
              else factory.disableRelocation();
              factory.stableTypeLimit(unlimitedPopulation ? 0 : atomicCapacity ? 2 : 2000);
            }
          );
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [DISCOVERY_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ProviderOptions; return new EvidenceStore(options.rid, options.evidenceFile);
      } },
      ProfileRequestHandler,
      SecondaryRequestHandler,
      ClientServerRequestHandler,
      ObjectRequestHandler,
      Config6InstanceSpot,
      Config6InstanceTimer,
      Config6ActorFactory,
      Config6ActorAdapter,
      Config6EntrySpot,
      Config6UserSpot,
      Config6UserSpotAdapter,
      Config6JoinHandler,
      Config6LeaveHandler,
      Config6ProbeHandler
    ]
  })(ProviderModule);
  return {
      moduleType: ProviderModule,
    disposeLocationStore: async () => {
      await locationStore?.dispose();
      locationStore = undefined;
      await relocationStore?.dispose();
      relocationStore = undefined;
    }
  };
}
