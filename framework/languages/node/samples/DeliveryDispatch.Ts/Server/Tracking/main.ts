import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createTrackingModule } from './tracking-module';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createTrackingModule(), {
    logger: false,
    abortOnError: false
  });
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
