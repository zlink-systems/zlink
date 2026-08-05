import 'reflect-metadata';
import { NestFactory } from '@nestjs/core';
import { createBingoMatchmakingModule } from './bingo-matchmaking-module';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';

async function main(): Promise<void> {
  const app = await NestFactory.createApplicationContext(createBingoMatchmakingModule(), {
    logger: ['error', 'warn', 'log']
  });
  await waitForShutdown();
  await closeNestRuntime(app);
}

void main();
