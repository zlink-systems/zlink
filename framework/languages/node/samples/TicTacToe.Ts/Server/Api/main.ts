import 'reflect-metadata';
import * as http from 'node:http';
import { NestFactory } from '@nestjs/core';
import { closeNestRuntime, waitForShutdown } from '../runtime-support';
import { TICTACTOE_SAMPLE_CONFIG } from '../Configuration/sample-config';
import type { TicTacToeSampleConfig } from '../Configuration/sample-config';
import { createTicTacToeApiModule, getCreateGameEndpoint } from './tictactoe-api-module';
import { CreateGameHttpReq } from '../../Shared/Contracts/messages';
import type { IncomingMessage, Server, ServerResponse } from 'node:http';

type HttpEndpoint = {
  host: string;
  port: number;
};

async function main(): Promise<void> {
  const TicTacToeApiModule = createTicTacToeApiModule();
  const apiApp = await NestFactory.createApplicationContext(TicTacToeApiModule, {
    logger: false,
    abortOnError: false
  });
  const config = apiApp.get<TicTacToeSampleConfig>(TICTACTOE_SAMPLE_CONFIG);
  const createGameReq = getCreateGameEndpoint(apiApp);

  const server = http.createServer(async (request: IncomingMessage, response: ServerResponse) => {
    if (request.method !== 'POST' || request.url !== '/games') {
      response.writeHead(404).end();
      return;
    }
    try {
      const body = parseCreateGameRequest(await readJson(request));
      const result = await createGameReq.handle(body);
      response.writeHead(200, { 'content-type': 'application/json' });
      response.end(JSON.stringify(result));
    } catch (error) {
      response.writeHead(500, { 'content-type': 'application/json' });
      response.end(JSON.stringify({ error: error instanceof Error ? error.message : String(error) }));
    }
  });
  await listen(server, config.apiHttpEndpoint);
  process.stdout.write(`${JSON.stringify({
    event: 'ready',
    endpoint: config.apiEndpoints[config.apiIndex],
    httpEndpoint: config.apiHttpEndpoint
  })}\n`);
  await waitForShutdown();
  await new Promise<void>((resolve, reject) => server.close((error) => error === undefined ? resolve() : reject(error)));
  await closeNestRuntime(apiApp);
}

function parseCreateGameRequest(value: unknown): CreateGameHttpReq {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('The request body must be a JSON object.');
  }
  const gameName: unknown = 'gameName' in value ? value.gameName : undefined;
  if (gameName !== undefined && typeof gameName !== 'string') {
    throw new Error('gameName must be a string.');
  }
  return new CreateGameHttpReq(gameName as string | undefined);
}

function readJson(request: IncomingMessage): Promise<unknown> {
  return new Promise<unknown>((resolve, reject) => {
    const chunks: Buffer[] = [];
    request.on('data', (chunk: Buffer) => chunks.push(chunk));
    request.once('error', reject);
    request.once('end', () => {
      try {
        resolve(JSON.parse(Buffer.concat(chunks).toString()));
      } catch (error) {
        reject(error);
      }
    });
  });
}

function listen(server: Server, endpoint: string): Promise<void> {
  const { host, port } = parseHttpEndpoint(endpoint);
  return new Promise<void>((resolve, reject) => {
    server.once('error', reject);
    server.listen(port, host, resolve);
  });
}

function parseHttpEndpoint(endpoint: string): HttpEndpoint {
  const url = new URL(endpoint);
  return { host: url.hostname, port: Number(url.port) };
}

main().catch((error: unknown) => {
  console.error(error);
  process.exitCode = 1;
});

export {};
