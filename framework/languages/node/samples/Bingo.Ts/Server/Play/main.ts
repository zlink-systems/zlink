import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_FRAMEWORK_RUNTIME } from '@zlink-systems/nestjs';
import {
  ZLinkFrameworkRelocationOutcome,
  ZLinkFrameworkRelocationMode,
  type ZLinkFrameworkRuntime
} from '@zlink-systems/framework';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import { createBingoPlayModule } from './bingo-play-module';
import { SampleNames } from '../Configuration/sample-names';
import { BINGO_SAMPLE_CONFIG } from '../Configuration/sample-config';
import type { BingoSampleConfig } from '../Configuration/sample-config';
async function bootstrap(): Promise<void> {
  const BingoPlayModule = createBingoPlayModule();
  const app = await NestFactory.createApplicationContext(BingoPlayModule, {
    logger: false,
    abortOnError: false
  });
  const config = app.get<BingoSampleConfig>(BINGO_SAMPLE_CONFIG);
  const frameworkRuntime = app.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME);
  const shutdown = new AbortController();
  const beginDrain = () => {
    console.log('bingo-drain requested');
    void frameworkRuntime.relocate({ mode: ZLinkFrameworkRelocationMode.PlannedMaintenance }).then((result) => {
      console.log(
        `bingo-drain result=${result.outcome === ZLinkFrameworkRelocationOutcome.Relocated
          ? 'drained'
          : 'blocked'} `
        + `outcome=${result.outcome} reason=${result.reason}`
      );
      process.removeListener('SIGUSR2', beginDrain);
      process.removeListener('SIGBREAK', beginDrain);
      shutdown.abort();
    }).catch((error) => {
      console.error('bingo-drain failed', error);
      process.exitCode = 1;
    });
  };
  process.once('SIGUSR2', beginDrain);
  process.once('SIGBREAK', beginDrain);

  process.stdout.write(`${JSON.stringify({
    event: 'ready',
    endpoint: config.playSpotEndpoint,
    meshName: SampleNames.playMeshName
  })}\n`);

  try {
    await waitForShutdown({ keepAlive: true, signal: shutdown.signal });
  } finally {
    console.log('bingo-play runtime closing');
    await closeNestRuntime(app);
    console.log('bingo-play runtime closed');
  }
}

bootstrap().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
