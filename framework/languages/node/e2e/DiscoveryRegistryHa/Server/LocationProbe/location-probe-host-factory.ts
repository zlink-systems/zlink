import fs from 'node:fs';
import { Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import { type ZLinkLocationRuntimeQuery } from '@zlink-systems/framework';
import { ZLINK_LOCATION_RUNTIME_QUERY, ZLinkModule, zlinkFramework } from '@zlink-systems/nestjs';
import { createRedisLocationStore } from '../../Shared/location-store';
import { validateLocationProbeOptions, type LocationProbeOptions } from './Configuration/location-probe-options';
import { DISCOVERY_OPTIONS, createDiscoveryConfigurationModule } from '../../configuration';
import { createLocationProbeEndpoints } from './Endpoints/location-probe-endpoints';
import { closeHttpServer, startHttpServer } from './Support/http-server';

export async function startLocationProbeHost(): Promise<void> {
  class LocationProbeModule {}
  const configuration = createDiscoveryConfigurationModule(validateLocationProbeOptions);
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [DISCOVERY_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as LocationProbeOptions;
          const builder = zlinkFramework();
          builder.addLocationStore(createRedisLocationStore(options));
          builder.addRouteMesh('profile').peerConnections();
          return builder.build();
        }
      })
    ]
  })(LocationProbeModule);
  const app = await NestFactory.createApplicationContext(LocationProbeModule, { logger: false, abortOnError: false });
  const options = app.get(DISCOVERY_OPTIONS, { strict: false }) as LocationProbeOptions;
  const locations = app.get(ZLINK_LOCATION_RUNTIME_QUERY, { strict: false }) as ZLinkLocationRuntimeQuery;
  fs.mkdirSync(options.logDir, { recursive: true });
  let stopping = false;

  const server = await startHttpServer(options.httpUrl, createLocationProbeEndpoints(options, locations, () => { stopping = true; }));
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}
