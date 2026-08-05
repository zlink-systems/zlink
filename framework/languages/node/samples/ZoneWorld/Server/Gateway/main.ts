import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZONEWORLD_CONFIG } from '../Configuration/configuration';
import type { ZoneWorldConfiguration } from '../Configuration/configuration';
import { closeRuntime, waitForShutdown } from '../runtime-support';
import { createGatewayModule } from './gateway-module';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createGatewayModule(), {
    logger: false,
    abortOnError: false
  });
  const config = app.get<ZoneWorldConfiguration>(ZONEWORLD_CONFIG);
  console.log(`topology=ready role=gateway endpoint=${config.gateway?.streamEndpoint ?? '-'}`);
  try {
    await waitForShutdown();
  } finally {
    await closeRuntime(app);
  }
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
