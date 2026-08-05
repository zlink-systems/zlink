import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createDispatchCenterModule } from '../DispatchCenter/dispatch-center-module';
import { startDispatchApi } from '../DispatchApi/dispatch-api-server';
import { EvidenceStore } from '../Configuration/evidence-store';
import { DELIVERYDISPATCH_SAMPLE_CONFIG } from '../Configuration/sample-config';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import type { DeliveryDispatchServerConfig } from '../Configuration/sample-config';

async function bootstrap(): Promise<void> {
  const center = await NestFactory.createApplicationContext(createDispatchCenterModule(), {
    logger: false,
    abortOnError: false
  });
  const config = center.get<DeliveryDispatchServerConfig>(DELIVERYDISPATCH_SAMPLE_CONFIG);
  const server = await startDispatchApi(center, config, new EvidenceStore(config.workDir));
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'dispatch' })}\n`);
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
