// SM-G3: Concurrent Join·Leave requests가 membership terminal을 하나씩 만든다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  AuthRes,
  EvidenceWaitReq,
  JoinUserSpotActorReq,
  JoinUserSpotActorRes,
  LeaveRes,
  LeaveReq,
  UserSpotAuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmG3(options: ClientOptions): Promise<void> {
  const actorCount = 2;
  const key = Date.now();
  const spotId = `spot-sm-g3-${key}`;
  const actorIds = Array.from({ length: actorCount }, (_, index) => `actor-sm-g3-${key}-${index}`);
  const clients: Array<ReturnType<typeof createStreamClient>> = [];

  try {
    for (const actorId of actorIds) {
      const client = await connectAndAuth(options.sessionAStreamEndpoint, spotId, actorId);
      clients.push(client);
    }

    await Promise.all(actorIds.map(async (actorId, index) => {
      const client = clients[index];
      const pingMsg = await client
        .request({ value: actorId } satisfies ActorPingReq)
        .packetName('UserActorPingReq')
        .timeout(5000)
        .submit<ActorPingRes>();
      ensure(pingMsg.actorId === actorId, 'SM-G3 actor request target mismatch.');
      ensure(pingMsg.nodeRid === 'play-a', 'SM-G3 actor request reached the wrong node.');

      const left = await client
        .request({ actorId } satisfies LeaveReq)
        .packetName('LeaveReq')
        .timeout(5000)
        .submit<LeaveRes>();
      ensure(left.accepted && left.actorId === actorId, 'SM-G3 leave reply mismatch.');
    }));

    const expectedEvidence = actorIds.flatMap((actorId) => [
      `spot-actor-joined|rid=play-a|spot=${spotId}|actor=${actorId}`,
      `spot-actor-left|rid=play-a|spot=${spotId}|actor=${actorId}`
    ]);
    const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: expectedEvidence,
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(
      expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
      'SM-G3 expected concurrent join and leave evidence.'
    );
    for (const actorId of actorIds) {
      ensure(
        evidence.filter((line) =>
          line.includes(`spot-actor-joined|rid=play-a|spot=${spotId}|actor=${actorId}`)).length === 1,
        `SM-G3 join evidence count mismatch for ${actorId}.`
      );
      ensure(
        evidence.filter((line) =>
          line.includes(`spot-actor-left|rid=play-a|spot=${spotId}|actor=${actorId}`)).length === 1,
        `SM-G3 leave evidence count mismatch for ${actorId}.`
      );
    }
  } finally {
    await Promise.all(clients.map((client) => client.close().catch(() => undefined)));
  }

  console.log('scenario SM-G3 passed');
}

async function connectAndAuth(endpoint: string, spotId: string, actorId: string) {
  const deadline = Date.now() + 15000;
  let last: unknown;
  while (Date.now() < deadline) {
    const client = createStreamClient(endpoint);
    try {
      await client.connect();
      await client
        .request({
          spotId,
          actorId,
          displayName: actorId,
          nodeRid: 'play-a'
        } satisfies UserSpotAuthReq)
        .packetName('UserSpotAuthReq')
        .timeout(5000)
        .submit<AuthRes>();
      await joinUserSpotActor(client, spotId, actorId);
      return client;
    } catch (error) {
      last = error;
      await client.close().catch(() => undefined);
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
  throw last instanceof Error
    ? new Error(`Actor auth did not become routable: ${actorId}. Last error: ${last.message}`)
    : new Error(`Actor auth did not become routable: ${actorId}.`);
}

async function joinUserSpotActor(
  client: ReturnType<typeof createStreamClient>,
  spotId: string,
  actorId: string
): Promise<void> {
  const joined = await client
    .request({ spotId, actorId } satisfies JoinUserSpotActorReq)
    .packetName('JoinUserSpotActorReq')
    .timeout(5000)
    .submit<JoinUserSpotActorRes>();
  ensure(joined.accepted && joined.actorId === actorId, `User spot actor join failed for ${actorId}.`);
}

function createStreamClient(endpoint: string) {
  return zlinkStreamConnectorFactory.create({
    endpoint,
    codec: zlinkStreamJsonCodec,
    dispatchMode: ZlinkStreamDispatchMode.Immediate,
    heartbeat: { enabled: false },
    waitTimeoutMs: 10000
  });
}
