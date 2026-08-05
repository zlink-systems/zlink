import fs from 'node:fs';
import { Injectable, Module } from '@nestjs/common';
import { NestFactory } from '@nestjs/core';
import type {
  ZLinkRouteClient,
  ZLinkRouteMeshRuntime,
  ZLinkSendHandler
} from '@zlink-systems/framework';
import {
  ZLINK_ROUTE_CLIENT,
  ZLINK_ROUTE_MESH_RUNTIME,
  ZLinkModule,
  zlinkFramework
} from '@zlink-systems/nestjs';
import { createRedisLocationStore, locationMessagingOptions } from '../../Shared/location-store';
import {
  REGISTRY_MESSAGING_OPTIONS,
  createRegistryMessagingConfigurationModule
} from '../../configuration';
import {
  validateObjectClientOptions,
  type ObjectClientOptions
} from './Configuration/object-client-options';
import { createObjectClientEndpoints } from './Endpoints/object-client-endpoints';
import { PacketNames, type ProfileMsg } from '../../Shared/messages';
import {
  closeHttpServer,
  startHttpServer
} from '../Provider/Support/http-server';

const meshName = 'registry.messaging.rm-a3';

@Injectable()
class ObjectClientProfileCommandHandler implements ZLinkSendHandler<ProfileMsg> {
  async handle(_command: ProfileMsg): Promise<void> {
    // The zero-weight phase only verifies Server membership and readiness.
  }
}

export async function startObjectClientHost(): Promise<void> {
  let stopping = false;
  const ObjectClientModule = createObjectClientModule();
  const app = await NestFactory.createApplicationContext(
    ObjectClientModule,
    { logger: false, abortOnError: false }
  );
  const options = app.get(
    REGISTRY_MESSAGING_OPTIONS,
    { strict: false }
  ) as ObjectClientOptions;
  const runtime = app.get(
    ZLINK_ROUTE_MESH_RUNTIME,
    { strict: false }
  ) as ZLinkRouteMeshRuntime;
  const route = app.get(ZLINK_ROUTE_CLIENT, { strict: false }) as ZLinkRouteClient;
  const server = await startHttpServer(
    options.httpUrl,
    createObjectClientEndpoints(
      options.rid,
      runtime,
      route,
      options.serverWeight,
      () => { stopping = true; }
    )
  );
  while (!stopping) {
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  await closeHttpServer(server);
  await app.close();
}

function createObjectClientModule(): Function {
  class ObjectClientModule {}
  const configuration = createRegistryMessagingConfigurationModule(
    validateObjectClientOptions
  );
  Module({
    imports: [
      configuration,
      ZLinkModule.forRootFactory({
        imports: [configuration],
        inject: [REGISTRY_MESSAGING_OPTIONS],
        useFactory: (value: unknown) => {
          const options = value as ObjectClientOptions;
          fs.mkdirSync(options.logDir, { recursive: true });
          const builder = zlinkFramework();
          builder.addLocationStore(createRedisLocationStore(options));
          locationMessagingOptions(builder.configureLocations());

          const mesh = builder.addRouteMesh(meshName)
            .listen(options.routeEndpoint)
            .routingId(options.rid);
          mesh.objects().client();
          const channel = mesh.channel(meshName);
          channel.client();
          if (options.serverWeight !== undefined) {
            channel.server()
              .setWeight(options.serverWeight)
              .addSendHandler(PacketNames.profileMsg, ObjectClientProfileCommandHandler);
          }
          for (let index = 0; index < options.routePeers.length; index += 1) {
            mesh.peerConnections().connect(
              options.routePeerRids[index]!,
              options.routePeers[index]!
            );
          }
          return builder.build();
        }
      })
    ],
    providers: [ObjectClientProfileCommandHandler]
  })(ObjectClientModule);
  return ObjectClientModule;
}
