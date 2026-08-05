// SM-E3: Idle timer가 explicit close를 시작한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotIdleCloseRes,
  SpotIdleCloseReq,
  SpotMissingTargetRes,
  SpotMissingTargetReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmE3(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-e3-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId && created.nodeRid === 'play-a', 'SM-E3 idle spot was not created on play-a.');

  const idle = await postJson<SpotIdleCloseRes>(options.playAUrl, '/spot/idle-close/start', {
    spotId,
    name: 'sm-e3-idle',
    periodMs: 50
  } satisfies SpotIdleCloseReq);
  ensure(idle.closed, 'SM-E3 idle close did not close the spot.');

  const closedSpotRequest = await postJson<SpotMissingTargetRes>(options.playAUrl, '/spot/missing-target/request', {
    spotId
  } satisfies SpotMissingTargetReq);
  ensure(closedSpotRequest.failed, 'SM-E3 closed spot request did not fail.');

  const expectedEvidence = [
    `timer-idle-close|rid=play-a|spot=${spotId}|name=sm-e3-idle|closed=True`,
    `spot-closing|rid=play-a|spot=${spotId}`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-E3 evidence mismatch.'
  );

  console.log('scenario SM-E3 passed');
}
