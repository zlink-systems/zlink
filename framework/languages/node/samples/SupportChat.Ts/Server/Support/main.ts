import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createSupportChatSupportModule } from './supportchat-support-module';
import { waitForShutdown } from '../runtime-support';
import { enableFlowFileLogging } from '../flow-file-logging';

enableFlowFileLogging('support');

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createSupportChatSupportModule(), {
    logger: false,
    abortOnError: false
  });
  process.stdout.write('supportchat-ready kind=public node=support\n');
  await waitForShutdown();
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
