import 'reflect-metadata';
import { enableFlowFileLogging } from '../flow-file-logging';
import { NestFactory } from '@nestjs/core';
import type { ZLinkFrameworkRuntime, ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_FRAMEWORK_RUNTIME, ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { closeNestRuntime, logSpotPeerReady, waitForShutdown } from '../runtime-support';
import { TICTACTOE_SAMPLE_CONFIG } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
import { SampleNames } from '../Configuration/sample-settings';
import { createTicTacToePlayModule } from './tictactoe-play-module';

enableFlowFileLogging('play');

async function main(): Promise<void> {
  const TicTacToePlayModule = createTicTacToePlayModule();
  const channelApp = await NestFactory.createApplicationContext(TicTacToePlayModule, {
    logger: false,
    abortOnError: false
  });
  const config = channelApp.get<TicTacToeSampleConfig>(TICTACTOE_SAMPLE_CONFIG);
  const routeMeshRuntime = channelApp.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME);
  const host = channelApp.get<ZLinkFrameworkRuntime>(ZLINK_FRAMEWORK_RUNTIME);
  void logSpotPeerReady(
    routeMeshRuntime,
    host,
    SampleNames.playSpotNode,
    config.instanceName,
    config.instanceName === 'play-a' ? 'play-b' : 'play-a'
  );
  process.stdout.write(
    `${JSON.stringify({
      event: 'ready',
      endpoint: config.playSpotEndpoint,
      spotEndpoint: config.playSpotEndpoint,
      streamEndpoint: config.playStreamEndpoint
    })}\n`
  );
  await waitForShutdown();
  await closeNestRuntime(channelApp);
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
