// SM-D3: Entry·User Spot Actor binding 의미가 같다 시나리오를 검증한다.
import {
  zlinkStreamConnectorFactory,
  zlinkStreamJsonCodec,
  ZlinkStreamDispatchMode
} from '@zlink-systems/stream-connector';
import type {
  ActorPingRes,
  ActorPingReq,
  ActorPushNotify,
  ActorPushReq,
  AuthRes,
  AuthReq,
  EvidenceWaitReq,
  JoinUserSpotActorReq,
  JoinUserSpotActorRes,
  UserSpotAuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmD3(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const entryActorId = `actor-sm-d3-entry-${suffix}`;
  const userSpotId = `spot-sm-d3-user-${suffix}`;
  const userActorId = `actor-sm-d3-user-${suffix}`;

  const entry = createStreamClient(options.sessionAStreamEndpoint);
  await entry.connect();
  try {
    await entry
      .request({
        actorId: entryActorId,
        displayName: 'entry bind',
        nodeRid: 'play-a'
      } satisfies AuthReq)
      .packetName('AuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${entryActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    const entryPushed = entry.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === entryActorId)
      .timeout(10000)
      .submit();
    const entryReply = await entry
      .request({ value: 'entry-push' } satisfies ActorPushReq)
      .packetName('ActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const entryNotify = await entryPushed;
    ensure(entryReply.actorId === entryActorId, 'SM-D3 entry bind actor mismatch.');
    ensure(entryReply.nodeRid === 'play-a', 'SM-D3 entry bind node mismatch.');
    ensure(entryNotify.payload.actorId === entryActorId, 'SM-D3 entry push actor mismatch.');
    ensure(entryNotify.payload.value === 'entry-push', 'SM-D3 entry push value mismatch.');
  } finally {
    await entry.close();
  }

  const user = createStreamClient(options.sessionAStreamEndpoint);
  await user.connect();
  try {
    await user
      .request({
        spotId: userSpotId,
        actorId: userActorId,
        displayName: 'user bind',
        nodeRid: 'play-a'
      } satisfies UserSpotAuthReq)
      .packetName('UserSpotAuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [`entry-joined|rid=play-a|actor=${userActorId}`],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    await joinUserSpotActor(user, userSpotId, userActorId);

    const userPushed = user.waitFor<ActorPushNotify>('ActorPushNotify')
      .where((message) => message.payload.actorId === userActorId)
      .timeout(10000)
      .submit();
    const userReply = await user
      .request({ value: 'user-relay' } satisfies ActorPingReq)
      .packetName('UserActorPingReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const userPushReply = await user
      .request({ value: 'user-push' } satisfies ActorPushReq)
      .packetName('UserActorPushReq')
      .timeout(5000)
      .submit<ActorPingRes>();
    const userNotify = await userPushed;

    ensure(userReply.actorId === userActorId, 'SM-D3 user bind actor mismatch.');
    ensure(userReply.nodeRid === 'play-a', 'SM-D3 user bind node mismatch.');
    ensure(userReply.spotId === userSpotId, 'SM-D3 user bind spot mismatch.');
    ensure(userReply.value === 'user-relay', 'SM-D3 user relay value mismatch.');
    ensure(userPushReply.actorId === userActorId, 'SM-D3 user push reply actor mismatch.');
    ensure(userNotify.payload.actorId === userActorId, 'SM-D3 user push actor mismatch.');
    ensure(userNotify.payload.value === 'user-push', 'SM-D3 user push value mismatch.');
  } finally {
    await user.close();
  }

  const expectedEvidence = [
    `spot-actor-joined|rid=play-a|spot=${userSpotId}|actor=${userActorId}`,
    `actor-pingMsg|rid=play-a|actor=${userActorId}|spot=${userSpotId}|value=user-relay`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-D3 expected user spot bind and relay evidence.'
  );

  console.log('scenario SM-D3 passed');
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
