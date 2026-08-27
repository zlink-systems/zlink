import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { createCourierActorNodeModule } from './courier-module';
import { closeNestRuntime, observeDeliveryRouteReadiness, waitForShutdown } from '../runtime-support';
import { DeliveryDispatchNodeIds, SampleNames } from '../../Shared/Configuration/sample-names';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(
    createCourierActorNodeModule({ courierId: 'courier-b' }),
    { logger: false, abortOnError: false }
  );
  observeDeliveryRouteReadiness(app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME), SampleNames.courierMeshName, DeliveryDispatchNodeIds.courierNode2);
  try {
    await waitForShutdown();
  } finally {
    await closeNestRuntime(app);
  }
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
