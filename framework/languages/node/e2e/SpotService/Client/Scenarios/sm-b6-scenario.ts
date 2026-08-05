// SM-B6: Explicit leave와 Session disconnect callback을 구분한다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
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

export async function runSmB6(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const spotId = `spot-sm-b6-${suffix}`;
  const leaveActorId = `actor-sm-b6-left-${suffix}`;
  const disconnectActorId = `actor-sm-b6-disconnected-${suffix}`;

  const leaveClient = createStreamClient(options.sessionAStreamEndpoint);
  await leaveClient.connect();
  try {
    await leaveClient
      .request({
        spotId,
        actorId: leaveActorId,
        displayName: leaveActorId,
        nodeRid: 'play-a'
      } satisfies UserSpotAuthReq)
      .packetName('UserSpotAuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${leaveActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    await joinUserSpotActor(leaveClient, spotId, leaveActorId);

    const left = await leaveClient
      .request({ actorId: leaveActorId } satisfies LeaveReq)
      .packetName('LeaveReq')
      .timeout(5000)
      .submit<LeaveRes>();
    ensure(left.accepted && left.actorId === leaveActorId, 'SM-B6 leave reply mismatch.');
  } finally {
    await leaveClient.close();
  }

  const expectedLeaveEvidence = [`spot-actor-left|rid=play-a|spot=${spotId}|actor=${leaveActorId}`];
  const playAAfterLeave = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedLeaveEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedLeaveEvidence.every((expected) => playAAfterLeave.some((line) => line.includes(expected))),
    'SM-B6 expected explicit leave evidence.'
  );
  ensure(
    playAAfterLeave.every((line) =>
      !line.includes(`spot-actor-disconnected|rid=play-a|spot=${spotId}|actor=${leaveActorId}`)),
    'SM-B6 explicit leave incorrectly emitted disconnect evidence.'
  );
  await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [`entry-disconnected|rid=play-a|actor=${leaveActorId}`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);

  const disconnectClient = createStreamClient(options.sessionAStreamEndpoint);
  await disconnectClient.connect();
  try {
    await disconnectClient
      .request({
        spotId,
        actorId: disconnectActorId,
        displayName: disconnectActorId,
        nodeRid: 'play-a'
      } satisfies UserSpotAuthReq)
      .packetName('UserSpotAuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${disconnectActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    await joinUserSpotActor(disconnectClient, spotId, disconnectActorId);
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`spot-actor-joined|rid=play-a|spot=${spotId}|actor=${disconnectActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
  } finally {
    await disconnectClient.close();
  }

  const expectedDisconnectEvidence = [
    `spot-actor-disconnected|rid=play-a|spot=${spotId}|actor=${disconnectActorId}`
  ];
  const playAAfterDisconnect = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedDisconnectEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedDisconnectEvidence.every((expected) => playAAfterDisconnect.some((line) => line.includes(expected))),
    'SM-B6 expected remote spot actor disconnect evidence.'
  );
  ensure(
    playAAfterDisconnect.every((line) =>
      !line.includes(`spot-actor-left|rid=play-a|spot=${spotId}|actor=${disconnectActorId}`)),
    'SM-B6 disconnect incorrectly emitted leave evidence.'
  );

  console.log('scenario SM-B6 passed');
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
