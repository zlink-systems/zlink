import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { createCourierSessionModule } from './courier-session-module';
import { closeNestRuntime, observeDeliveryRouteReadiness, waitForShutdown } from '../runtime-support';
import { SampleNames } from '../../Shared/Configuration/sample-names';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createCourierSessionModule(), {
    logger: false,
    abortOnError: false
  });
  observeDeliveryRouteReadiness(app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME), SampleNames.courierMeshName, 'courier-session');
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'courier-session' })}\n`);
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
