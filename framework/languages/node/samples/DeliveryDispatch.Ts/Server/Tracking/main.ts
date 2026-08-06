import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { createTrackingModule } from './tracking-module';
import { closeNestRuntime, observeDeliveryRouteReadiness, waitForShutdown } from '../runtime-support';
import { SampleNames } from '../../Shared/Configuration/sample-names';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createTrackingModule(), {
    logger: false,
    abortOnError: false
  });
  observeDeliveryRouteReadiness(app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME), SampleNames.customerMeshName, 'tracking');
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'tracking' })}\n`);
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
