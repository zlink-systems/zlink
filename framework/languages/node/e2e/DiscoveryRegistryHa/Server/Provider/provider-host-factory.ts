import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import {
  ZLinkMessageFlowLogMode,
  type ZLinkRouteMeshRuntimeOptions,
  type ZLinkFrameworkRuntime
} from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_MESH_RUNTIME_OPTIONS,
  ZLINK_FRAMEWORK_RUNTIME,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { ChannelNames, PacketNames } from '../../Shared/messages';
import { configureStoreFailureLocationOptions, createRedisLocationStore } from '../../Shared/location-store';
import type { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { validateProviderOptions } from './Configuration/provider-options';
import type { ProviderOptions } from './Configuration/provider-options';
import { DISCOVERY_OPTIONS, createDiscoveryConfigurationModule } from '../../configuration';
import { createProviderEndpoints } from './Endpoints/provider-endpoints';
import { ProfileRequestHandler } from './Handlers/profile-request-handler';
import { EvidenceStore } from './Infrastructure/evidence-store';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startProviderHost(): Promise<void> {
  let stopping = false;

  const provider = createProviderModule();
  const app = await NestFactory.createApplicationContext(provider.moduleType, { logger: false, abortOnError: false });
  const options = app.get(DISCOVERY_OPTIONS, { strict: false }) as ProviderOptions;
  const evidence = app.get(EvidenceStore, { strict: false });
  const runtimeOptions = app.get(ZLINK_ROUTE_MESH_RUNTIME_OPTIONS, { strict: false }) as ZLinkRouteMeshRuntimeOptions;
  const frameworkRuntime = app.get(ZLINK_FRAMEWORK_RUNTIME, { strict: false }) as ZLinkFrameworkRuntime;
  const server = await startHttpServer(
    options.httpUrl,
    createProviderEndpoints(evidence, runtimeOptions, frameworkRuntime, () => { stopping = true; })
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
  class ProviderModule {}
  const configuration = createDiscoveryConfigurationModule(validateProviderOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration], inject: [DISCOVERY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ProviderOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder
            .configureDispatch()
              .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
              .traceLogFile(`${options.logDir}/${options.rid}-flow.log`)
              .traceLabel(options.rid);
          locationStore = createRedisLocationStore(options);
          builder.addLocationStore(locationStore);
          configureStoreFailureLocationOptions(builder.configureLocations());
          const profile = builder.addRouteMesh(ChannelNames.profile)
            .listen(options.channelEndpoint)
            .routingId(options.rid);
          profile.channel(ChannelNames.profile).server()
            .addRequestHandler(PacketNames.profileReq, ProfileRequestHandler);
          return builder.build();
        }
      })
    ],
    providers: [
      { provide: EvidenceStore, inject: [DISCOVERY_OPTIONS], useFactory: (value: unknown) => {
        const options = value as ProviderOptions; return new EvidenceStore(options.rid, options.evidenceFile);
      } },
      ProfileRequestHandler
    ]
  })(ProviderModule);
  return {
    moduleType: ProviderModule,
    disposeLocationStore: async () => {
      await locationStore?.dispose();
      locationStore = undefined;
    }
  };
}
