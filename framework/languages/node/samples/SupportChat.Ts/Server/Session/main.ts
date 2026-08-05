import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createSupportChatSessionModule } from './supportchat-session-module';
import { waitForShutdown } from '../runtime-support';

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createSupportChatSessionModule(), {
    logger: false,
    abortOnError: false
  });
  process.stdout.write(`${JSON.stringify({ event: 'ready', role: 'session' })}\n`);
  await waitForShutdown();
  await app.close();
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});
