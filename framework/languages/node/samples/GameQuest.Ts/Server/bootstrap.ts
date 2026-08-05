import 'reflect-metadata';
import http from 'node:http';
import { setTimeout as delay } from 'node:timers/promises';
import { NestFactory } from '@nestjs/core';
import type { ZLinkRouteMeshRuntime } from '@zlink-systems/framework';
import { ZLINK_ROUTE_MESH_RUNTIME } from '@zlink-systems/nestjs';
import { createGameApiModule } from './GameApi/game-api-module';
import { startGameApiServer } from './GameApi/game-api-server';
import { createQuestMissionModule } from './QuestMission/gamequest-quest-module';
import { QuestEventStore } from './Shared/Store/quest-progress-store';
import { PlayerQuestSpotProvisioner } from './QuestMission/Infrastructure/ZLink/player-quest-spot-provisioner';
import { GAMEQUEST_LOCATION_STORE } from './Configuration/tokens';
import { GAMEQUEST_SAMPLE_CONFIG } from './Configuration/sample-config';
import type { GameQuestServerConfig } from './Configuration/sample-config';
import type { ZLinkRedisLocationStore } from '@zlink-systems/framework-locations-redis';
import { SampleNames } from '../Shared/Configuration/sample-names';

type GameQuestRole = 'api-a' | 'api-b' | 'mission-a' | 'mission-b';

async function bootstrapGameQuest(role: GameQuestRole): Promise<void> {
  const isApi = role === 'api-a' || role === 'api-b';
  const moduleType = isApi ? createGameApiModule(role) : createQuestMissionModule(role);
  const app = await NestFactory.createApplicationContext(moduleType, {
    logger: false,
    abortOnError: true
  });
  const config = app.get<GameQuestServerConfig>(GAMEQUEST_SAMPLE_CONFIG);
  if (!isApi) {
    await waitForRouteMeshReady(
      app.get<ZLinkRouteMeshRuntime>(ZLINK_ROUTE_MESH_RUNTIME),
      SampleNames.playerQuestSpotMesh
    );
  }
  const httpServer = isApi
    ? await startGameApiServer(app, config, role)
    : await startMissionSelfCheckServer(config, role, app.get(PlayerQuestSpotProvisioner));

  process.stdout.write(`${JSON.stringify({ event: 'ready', role })}\n`);
  try {
    await waitForShutdown();
  } finally {
    await new Promise<void>((resolve) => httpServer.close(() => resolve()));
    await closeNestRuntime(app);
    await app.get<ZLinkRedisLocationStore>(GAMEQUEST_LOCATION_STORE).dispose();
  }
}

async function waitForRouteMeshReady(
  runtime: ZLinkRouteMeshRuntime,
  meshName: string
): Promise<void> {
  const deadline = Date.now() + 30_000;
  for (;;) {
    const status = runtime.snapshot(meshName);
    if (status.isReady && status.placement.isAvailable) return;
    if (Date.now() >= deadline) {
      throw new Error(
        `GameQuest RouteMesh '${meshName}' did not become ready before startup deadline.`
      );
    }
    // The HTTP health endpoint is published only after this barrier so the
    // sample client cannot submit an Instance intent before placement exists.
    await delay(10);
  }
}

function startMissionSelfCheckServer(
  config: GameQuestServerConfig,
  role: 'mission-a' | 'mission-b',
  playerQuests: PlayerQuestSpotProvisioner
): Promise<http.Server> {
  const store = new QuestEventStore(config.workDir);
  const missionUrl = role === 'mission-a' ? config.missionAHttpUrl : config.missionBHttpUrl;
  const url = new URL(missionUrl);
  const server = http.createServer(async (request, response) => {
    const ownerMatch = request.url?.match(/^\/self-check\/owner\/([^/]+)\/close$/);
    if (request.method === 'GET' && request.url === '/health') {
      sendJson(response, 200, { ready: true, role });
      return;
    }
    if (request.method === 'GET' && request.url === '/self-check/events') {
      sendJson(response, 200, store.readQuestEventNames());
      return;
    }
    if (request.method === 'POST' && ownerMatch !== undefined && ownerMatch !== null) {
      const playerId = decodeURIComponent(ownerMatch[1]);
      const closed = await playerQuests.deactivate(playerId);
      if (closed) store.closeOwner(playerId, role);
      sendJson(response, 200, { closed });
      return;
    }
    sendJson(response, 404, { error: 'not-found' });
  });
  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(url.port), url.hostname, () => {
      server.off('error', reject);
      resolve(server);
    });
  });
}

function sendJson(response: http.ServerResponse, statusCode: number, body: unknown): void {
  response.writeHead(statusCode, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

function waitForShutdown(): Promise<void> {
  return new Promise((resolve) => {
    const keepAlive = setInterval(() => undefined, 60_000);
    const stop = (): void => {
      clearInterval(keepAlive);
      resolve();
    };
    process.once('SIGINT', stop);
    process.once('SIGTERM', stop);
  });
}

async function closeNestRuntime(container: { close(): Promise<void> }): Promise<void> {
  try {
    await container.close();
  } catch (error) {
    const candidate = error as { name?: string; code?: number };
    if (candidate.name === 'CloseError' && [0, 401].includes(candidate.code ?? -1)) return;
    throw error;
  }
}

export { bootstrapGameQuest };
export type { GameQuestRole };
