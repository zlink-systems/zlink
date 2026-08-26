import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { createDispatchCenterModule } from '../DispatchCenter/dispatch-center-module';
import { startDispatchApi } from '../DispatchApi/dispatch-api-server';
import { EvidenceStore } from '../Configuration/evidence-store';
import { DELIVERYDISPATCH_SAMPLE_CONFIG } from '../Configuration/sample-config';
import { closeNestRuntime, observeDeliveryRouteReadiness, waitForShutdown } from '../runtime-support';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';
import { DeliveryDispatchNodeIds, SampleNames } from '../../Shared/Configuration/sample-names';

async function bootstrap(): Promise<void> {
  const center = await NestFactory.createApplicationContext(createDispatchCenterModule(), {
    logger: false,
    abortOnError: false
  });
  observeDeliveryRouteReadiness(
    center.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME),
    SampleNames.courierMeshName,
    DeliveryDispatchNodeIds.dispatch,
    [DeliveryDispatchNodeIds.courierNode1, DeliveryDispatchNodeIds.courierNode2]
  );
  const config = center.get<DeliveryDispatchServerConfig>(DELIVERYDISPATCH_SAMPLE_CONFIG);
  const server = await startDispatchApi(center, config, new EvidenceStore(config.workDir));
  try {
    await waitForShutdown();
  } finally {
    await new Promise<void>((resolve) => server.close(() => resolve()));
    await closeNestRuntime(center);
  }
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
