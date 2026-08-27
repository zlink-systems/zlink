import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { createSupportChatSessionModule } from './supportchat-session-module';
import { SampleNames } from '../Configuration/sample-names';
import { waitForRouteMeshReady, waitForShutdown } from '../runtime-support';
import { enableFlowFileLogging } from '../flow-file-logging';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';

enableFlowFileLogging('session');

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createSupportChatSessionModule(), {
    logger: false,
    abortOnError: false
  });
  await waitForRouteMeshReady(
    app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME),
    SampleNames.conversationSpotMesh
  );
  process.stdout.write(
    `supportchat-ready kind=spot-route node=session mesh=${SampleNames.conversationSpotMesh}\n`
  );
  process.stdout.write('supportchat-ready kind=stream node=session\n');
  await waitForShutdown();
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
