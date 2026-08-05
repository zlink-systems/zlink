import http from 'node:http';
import { ZLINK_SPOT_OUTBOUND } from '@zlink-systems/nestjs';
import { questMissionSpotId, SampleNames } from '../../Shared/Configuration/sample-names';
import { GameQuestSelfCheckStore, GameplayStateStore } from '../Shared/Store/quest-progress-store';
import {
  deleteQuestProjectionReq,
  rebuildQuestProjectionReq
} from '../../Shared/Contracts/messages';
import type { INestApplicationContext } from '@nestjs/common';
import type { ZLinkSpotOutbound } from '@zlink-systems/framework';
import type { GameQuestServerConfig } from '../Configuration/sample-config';
import type {
  DeleteQuestProjectionRes,
  GameQuestServerAssertRes,
  GetGameplaySnapshotReq,
  QuestProgress
} from '../../Shared/Contracts/messages';

function startGameApiServer(
  app: INestApplicationContext,
  config: GameQuestServerConfig,
  instanceId: 'api-a' | 'api-b'
): Promise<http.Server> {
  const gameplayState = app.get(GameplayStateStore, { strict: false });
  const selfCheck = app.get(GameQuestSelfCheckStore, { strict: false });
  const spots = app.get(ZLINK_SPOT_OUTBOUND, { strict: false }) as ZLinkSpotOutbound;
  const base = new URL(instanceId === 'api-a' ? config.apiAHttpUrl : config.apiBHttpUrl);
  const server = http.createServer(async (request, response) => {
    try {
      if (request.method === 'GET' && request.url === '/health') {
        sendJson(response, 200, { ready: true, role: instanceId });
        return;
      }
      if (request.method === 'POST' && request.url === '/internal/snapshot') {
        const body = await readJson<GetGameplaySnapshotReq>(request);
        sendJson(response, 200, gameplayState.readGameplaySnapshot(body.playerId));
        return;
      }
      const deleteMatch = request.url?.match(/^\/self-check\/projection\/([^/]+)\/([^/]+)\/delete$/);
      if (request.method === 'POST' && deleteMatch !== undefined && deleteMatch !== null) {
        const playerId = decodeURIComponent(deleteMatch[1]);
        const questId = decodeURIComponent(deleteMatch[2]);
        const deleted = await spots
          .requestToSpot(questMissionSpotId(playerId), deleteQuestProjectionReq(playerId, questId))
          .instanceSpot(SampleNames.playerQuestSpotType)
          .inMesh(SampleNames.playerQuestSpotMesh)
          .timeout(SampleNames.requestTimeout)
          .submit<DeleteQuestProjectionRes>();
        sendJson(response, 200, deleted);
        return;
      }
      const rebuildMatch = request.url?.match(/^\/self-check\/projection\/([^/]+)\/([^/]+)\/rebuild$/);
      if (request.method === 'POST' && rebuildMatch !== undefined && rebuildMatch !== null) {
        const playerId = decodeURIComponent(rebuildMatch[1]);
        const questId = decodeURIComponent(rebuildMatch[2]);
        const rebuilt = await spots
          .requestToSpot(questMissionSpotId(playerId), rebuildQuestProjectionReq(playerId, questId))
          .instanceSpot(SampleNames.playerQuestSpotType)
          .inMesh(SampleNames.playerQuestSpotMesh)
          .timeout(SampleNames.requestTimeout)
          .submit<QuestProgress>();
        sendJson(response, 200, rebuilt);
        return;
      }
      const missedMatch = request.url?.match(/^\/self-check\/gameplay\/kill-without-publish\/([^/]+)$/);
      if (request.method === 'POST' && missedMatch !== undefined && missedMatch !== null) {
        gameplayState.killWithoutPublish(decodeURIComponent(missedMatch[1]));
        sendJson(response, 200, { accepted: true });
        return;
      }
      if (request.method === 'POST' && request.url === '/self-check/assert') {
        sendJson(response, 200, selfCheck.assertServerEvidence() satisfies GameQuestServerAssertRes);
        return;
      }
      sendJson(response, 404, { error: 'not-found' });
    } catch (error) {
      sendJson(response, 500, { error: error instanceof Error ? error.message : String(error) });
    }
  });

  return new Promise((resolve, reject) => {
    server.once('error', reject);
    server.listen(Number(base.port), base.hostname, () => {
      server.off('error', reject);
      resolve(server);
    });
  });
}

function readJson<T>(request: http.IncomingMessage): Promise<T> {
  return new Promise((resolve, reject) => {
    const chunks: Buffer[] = [];
    request.on('data', (chunk) => chunks.push(Buffer.from(chunk)));
    request.on('error', reject);
    request.on('end', () => {
      try {
        const text = Buffer.concat(chunks).toString('utf8');
        resolve(JSON.parse(text.length === 0 ? '{}' : text) as T);
      } catch (error) {
        reject(error);
      }
    });
  });
}

function sendJson(response: http.ServerResponse, statusCode: number, body: unknown): void {
  response.writeHead(statusCode, { 'content-type': 'application/json' });
  response.end(JSON.stringify(body));
}

export { startGameApiServer };
