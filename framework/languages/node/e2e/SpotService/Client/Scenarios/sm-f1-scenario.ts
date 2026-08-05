// SM-F1: Same-node Spot direct request와 send를 처리한다 시나리오를 검증한다.
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

export async function runSmF1(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-f1-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-F1 did not create the requested spot.');
  ensure(created.nodeRid === 'play-a', 'SM-F1 created spot on the wrong node.');

  const state = await postJson<StateRes>(options.playAUrl, '/spot/state/request', {
    spotId,
    operation: 'add',
    delta: 7
  } satisfies SpotStateRouteReq);
  ensure(state.spotId === spotId, 'SM-F1 request reached the wrong spot.');
  ensure(state.nodeRid === 'play-a', 'SM-F1 request reached the wrong node.');
  ensure(state.value === 7, 'SM-F1 state reply mismatch.');

  const command = await postJson<SpotStateMsgRes>(options.playAUrl, '/spot/state/command', {
    spotId,
    marker: 'sm-f1-command'
  } satisfies SpotStateMsgReq);
  ensure(command.spotId === spotId && command.accepted, 'SM-F1 command was not accepted.');

  const expectedEvidence = [
    `spot-state-request|rid=play-a|spot=${spotId}|value=7`,
    `spot-state-command|rid=play-a|spot=${spotId}|marker=sm-f1-command`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-F1 evidence mismatch.'
  );

  console.log('scenario SM-F1 passed');
}
