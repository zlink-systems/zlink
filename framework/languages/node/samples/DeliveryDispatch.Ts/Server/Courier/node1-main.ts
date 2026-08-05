import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createCourierActorNodeModule } from './courier-module';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';

async function bootstrap(): Promise<void> {
  const app = await NestFactory.createApplicationContext(
    createCourierActorNodeModule({ courierId: 'courier-a' }),
    { logger: false, abortOnError: false }
  );
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'courier-spot-node1' })}\n`);
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
