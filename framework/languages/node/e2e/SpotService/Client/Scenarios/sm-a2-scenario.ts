// SM-A2: User Spot state를 serial하게 변경한다 시나리오를 검증한다.
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

export async function runSmA2(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a2-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-A2 did not create the requested spot.');

  const reply = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
    spotId,
    operation: 'add',
    delta: 1
  } satisfies SpotStateRouteReq);
  ensure(reply.spotId === spotId, 'SM-A2 state request reached the wrong spot.');
  ensure(reply.nodeRid === 'play-a', 'SM-A2 state request reached the wrong node.');
  ensure(reply.value === 1, 'SM-A2 state mutation reply mismatch.');

  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [`spot-state-request|rid=play-a|spot=${spotId}|value=1`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    evidence.some((line) => line.includes(`spot-state-request|rid=play-a|spot=${spotId}|value=1`)),
    'SM-A2 state mutation evidence missing.'
  );

  console.log('scenario SM-A2 passed');
}
