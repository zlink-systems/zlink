// SM-B9: Target Spot의 Join accept와 reject를 구분한다 시나리오를 검증한다.
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
  UserSpotAuthReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmB9(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const spotId = `spot-sm-b9-${suffix}`;
  const acceptedActorId = `actor-sm-b9-accepted-${suffix}`;
  const rejectedActorId = `actor-sm-b9-reject-${suffix}`;

  const local = createStreamClient(options.sessionAStreamEndpoint);
  await local.connect();
  try {
    await local
      .request({
        spotId,
        actorId: acceptedActorId,
        displayName: acceptedActorId,
        nodeRid: 'play-a'
      } satisfies UserSpotAuthReq)
      .packetName('UserSpotAuthReq')
      .timeout(5000)
      .submit<AuthRes>();
    const accepted = await local
      .request({ spotId, actorId: acceptedActorId } satisfies JoinUserSpotActorReq)
      .packetName('JoinUserSpotActorReq')
      .timeout(5000)
      .submit<JoinUserSpotActorRes>();
    ensure(accepted.accepted && accepted.actorId === acceptedActorId, 'SM-B9 local accepted join mismatch.');

    const remoteRejected = createStreamClient(options.sessionAStreamEndpoint);
    await remoteRejected.connect();
    try {
      await remoteRejected
        .request({
          spotId,
          actorId: rejectedActorId,
          displayName: rejectedActorId,
          nodeRid: 'play-b'
        } satisfies UserSpotAuthReq)
        .packetName('UserSpotAuthReq')
        .timeout(5000)
        .submit<AuthRes>();
      const rejected = await remoteRejected
        .request({ spotId, actorId: rejectedActorId } satisfies JoinUserSpotActorReq)
        .packetName('JoinUserSpotActorReq')
        .timeout(5000)
        .submit<JoinUserSpotActorRes>();
      ensure(!rejected.accepted && rejected.actorId === rejectedActorId, 'SM-B9 rejected join was accepted.');
    } finally {
      await remoteRejected.close();
    }

    const acceptedEvidence = `spot-actor-joined|rid=play-a|spot=${spotId}|actor=${acceptedActorId}`;
    const rejectedEvidence = `spot-actor-join-rejected|rid=play-b|spot=${spotId}|actor=${rejectedActorId}`;
    const playA = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
      containsAll: [acceptedEvidence],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    const playB = await postJson<string[]>(options.playBUrl, '/evidence/wait', {
      containsAll: [rejectedEvidence],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);
    ensure(playA.some((line) => line.includes(acceptedEvidence)), 'SM-B9 accepted admission evidence mismatch.');
    ensure(
      playB.some((line) => line.includes(rejectedEvidence)),
      'SM-B9 rejected admission evidence mismatch.'
    );
    ensure(
      playB.every((line) => !line.includes(`spot-actor-joined|rid=play-b|spot=${spotId}|actor=${rejectedActorId}`)),
      'SM-B9 rejected actor joined the target spot.'
    );
  } finally {
    await local.close();
  }

  console.log('scenario SM-B9 passed');
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
