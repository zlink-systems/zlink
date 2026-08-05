// SM-A5: Application Stage wrapper가 Spot 계약을 바꾸지 않는다 시나리오를 검증한다.
import type {
  CloseSpotRes,
  CloseSpotReq,
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotStageProbeReq,
  SpotStageTimerRes,
  SpotStageTimerReq,
  SpotStateRouteReq,
  StateRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA5(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a5-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-A5 did not create the requested spot.');
  ensure(created.nodeRid === 'play-a', 'SM-A5 created spot on the wrong node.');

  const ready = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
    spotId,
    operation: 'noop',
    delta: 0
  } satisfies SpotStateRouteReq);
  ensure(
    ready.spotId === spotId && ready.nodeRid === 'play-a',
    'SM-A5 spot route did not become ready.'
  );

  const probeReply = await postJson<StateRes>(options.playAUrl, '/spot/stage/request', {
    spotId,
    marker: 'sm-a5-stage',
    delta: 9
  } satisfies SpotStageProbeReq);
  ensure(probeReply.spotId === spotId, 'SM-A5 stage request reached the wrong spot.');
  ensure(probeReply.nodeRid === 'play-a', 'SM-A5 stage request reached the wrong node.');
  ensure(probeReply.value === 9, 'SM-A5 stage request state mismatch.');

  const timer = await postJson<SpotStageTimerRes>(options.playAUrl, '/spot/stage/timer', {
    spotId,
    name: 'sm-a5-stage-timer',
    periodMs: 50
  } satisfies SpotStageTimerReq);
  ensure(timer.spotId === spotId && timer.started, 'SM-A5 stage timer was not started.');

  const closeReply = await postJson<CloseSpotRes>(options.playAUrl, '/spot/close', {
    spotId
  } satisfies CloseSpotReq);
  ensure(closeReply.closed, 'SM-A5 did not close the spot.');

  const expectedEvidence = [
    `spot-initialize|rid=play-a|spot=${spotId}`,
    `stage-request|rid=play-a|spot=${spotId}|marker=sm-a5-stage|value=9`,
    `stage-timer|rid=play-a|spot=${spotId}|name=sm-a5-stage-timer`,
    `spot-closing|rid=play-a|spot=${spotId}`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-A5 evidence mismatch.'
  );

  console.log('scenario SM-A5 passed');
}
