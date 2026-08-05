// SM-F2: 다른 MeshNode의 Spot을 SpotId로 호출한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotStateMsgRes,
  SpotStateMsgReq,
  SpotStateRouteReq,
  StateRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmF2(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-f2-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-F2 did not create the requested spot.');
  ensure(created.nodeRid === 'play-a', 'SM-F2 created spot on the wrong node.');

  const state = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
    spotId,
    operation: 'add',
    delta: 5
  } satisfies SpotStateRouteReq);
  ensure(state.spotId === spotId, 'SM-F2 request reached the wrong spot.');
  ensure(state.nodeRid === 'play-a', 'SM-F2 request reached the wrong node.');
  ensure(state.value === 5, 'SM-F2 state reply mismatch.');

  const command = await postJson<SpotStateMsgRes>(options.playAUrl, '/spot/state/command', {
    spotId,
    marker: 'sm-f2-command'
  } satisfies SpotStateMsgReq);
  ensure(command.spotId === spotId && command.accepted, 'SM-F2 command was not accepted.');

  const expectedEvidence = [
    `spot-state-request|rid=play-a|spot=${spotId}|value=5`,
    `spot-state-command|rid=play-a|spot=${spotId}|marker=sm-f2-command`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-F2 evidence mismatch.'
  );

  console.log('scenario SM-F2 passed');
}
