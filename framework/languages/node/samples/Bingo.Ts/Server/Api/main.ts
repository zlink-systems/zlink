import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import { createBingoApiModule } from './bingo-api-module';
import { BINGO_SAMPLE_CONFIG } from '../Configuration/sample-config';
import { SampleNames } from '../Configuration/sample-names';
import type { BingoSampleConfig } from '../Configuration/sample-config';
async function bootstrap(): Promise<void> {
  const BingoApiModule = createBingoApiModule();
  const app = await NestFactory.createApplicationContext(BingoApiModule, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<BingoSampleConfig>(BINGO_SAMPLE_CONFIG);
  process.stdout.write(`${JSON.stringify({
    event: 'ready',
    endpoint: config.apiEndpoint,
    channelName: SampleNames.apiChannel
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
