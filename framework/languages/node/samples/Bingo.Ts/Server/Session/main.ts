import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import { createBingoSessionModule } from './bingo-session-module';
import { BingoSession } from './Sessions/bingo-session';
import { BINGO_SAMPLE_CONFIG } from '../Configuration/sample-config';
import type { BingoSampleConfig } from '../Configuration/sample-config';

async function bootstrap(): Promise<void> {
  const BingoSessionModule = createBingoSessionModule();
  const app = await NestFactory.createApplicationContext(BingoSessionModule, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<BingoSampleConfig>(BINGO_SAMPLE_CONFIG);
  process.stdout.write(`${JSON.stringify({
    event: 'ready',
    endpoint: config.sessionEndpoint,
    stream: BingoSession.name
  })}\n`);

  try {
    await waitForShutdown({ keepAlive: true });
  } finally {
    await closeNestRuntime(app);
  }
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
