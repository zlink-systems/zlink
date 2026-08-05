import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLinkPeerState, type ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import { TICTACTOE_SAMPLE_CONFIG } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
import { SampleNames } from '../Configuration/sample-settings';
import { createTicTacToePlayModule } from './tictactoe-play-module';

async function main(): Promise<void> {
  const TicTacToePlayModule = createTicTacToePlayModule();
  const channelApp = await NestFactory.createApplicationContext(TicTacToePlayModule, {
    logger: false,
    abortOnError: false
  });
  const config = channelApp.get<TicTacToeSampleConfig>(TICTACTOE_SAMPLE_CONFIG);
  const routeMeshRuntime = channelApp.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME);
  void logSpotPeerReady(routeMeshRuntime, SampleNames.playSpotNode);
  process.stdout.write(`${JSON.stringify({
    event: 'ready',
    endpoint: config.playSpotEndpoint,
    spotEndpoint: config.playSpotEndpoint,
    streamEndpoint: config.playStreamEndpoint
  })}\n`);
  await waitForShutdown();
  await closeNestRuntime(channelApp);
}

async function logSpotPeerReady(runtime: ZLinkRouteMeshRuntime, meshName: string): Promise<void> {
  const hasReadyPeer = (): boolean => runtime.snapshot(meshName).peers.some(
    (peer) => peer.state === ZLinkPeerState.Ready
  );
  if (hasReadyPeer()) {
    process.stdout.write(`${JSON.stringify({ event: 'spotPeerReady' })}\n`);
    return;
  }

  const signal = AbortSignal.timeout(3000);
  try {
    for await (const _event of runtime.observe(meshName, 64, signal)) {
      if (!hasReadyPeer()) continue;
      process.stdout.write(`${JSON.stringify({ event: 'spotPeerReady' })}\n`);
      return;
    }
  } catch (error: unknown) {
    if (!signal.aborted) throw error;
  }
  process.stderr.write('Timed out waiting for a ready Play RouteMesh peer.\n');
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
