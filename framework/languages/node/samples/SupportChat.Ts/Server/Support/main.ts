import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createSupportChatSupportModule } from './supportchat-support-module';
import { waitForShutdown } from '../runtime-support';

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createSupportChatSupportModule(), {
    logger: false,
    abortOnError: false
  });
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'support' })}\n`);
  await waitForShutdown();
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
