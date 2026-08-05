// SM-A3: Global SpotId가 정확한 Spot에 도달한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotStateRouteReq,
  StateRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA3(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a3-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    created.spotId === spotId && created.nodeRid === 'play-a',
    'SM-A3 routed spot was not created on play-a.'
  );

  const routeReply = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
    spotId,
    operation: 'add',
    delta: 1
  } satisfies SpotStateRouteReq);
  ensure(routeReply.spotId === spotId, 'SM-A3 route reached the wrong spot.');
  ensure(routeReply.nodeRid === 'play-a', 'SM-A3 route reached the wrong node.');
  ensure(routeReply.value === 1, 'SM-A3 state reply mismatch.');

  const expectedPlayAEvidence = [`spot-state-request|rid=play-a|spot=${spotId}|value=1`];
  const playAEvidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedPlayAEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedPlayAEvidence.every((expected) => playAEvidence.some((line) => line.includes(expected))),
    'SM-A3 evidence mismatch.'
  );

  const playBEvidence = await postJson<string[]>(options.playBUrl, '/evidence/wait', {
    containsAll: [],
    timeoutMilliseconds: 100
  } satisfies EvidenceWaitReq);
  ensure(
    playBEvidence.every((line) => !line.includes(`spot-state-request|rid=play-b|spot=${spotId}`)),
    'SM-A3 route resolver leaked the spot request to play-b.'
  );

  console.log('scenario SM-A3 passed');
}
