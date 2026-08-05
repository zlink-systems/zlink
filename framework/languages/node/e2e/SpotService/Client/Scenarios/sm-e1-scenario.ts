// SM-E1: Handler 없는 Spot request를 관찰한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotMissingMsgRes,
  SpotMissingMsgReq,
  SpotMissingHandlerRes,
  SpotMissingHandlerReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmE1(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-e1-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId && created.nodeRid === 'play-a', 'SM-E1 spot was not created on play-a.');

  const missingRequest = await postJson<SpotMissingHandlerRes>(options.playAUrl, '/spot/missing-handler/request', {
    spotId
  } satisfies SpotMissingHandlerReq);
  ensure(missingRequest.failed, 'SM-E1 missing handler request did not fail.');

  const missingCommand = await postJson<SpotMissingMsgRes>(options.playAUrl, '/spot/missing-handler/command', {
    spotId,
    marker: 'missing-command'
  } satisfies SpotMissingMsgReq);
  ensure(missingCommand.sent, 'SM-E1 missing handler command was not sent.');

  const expectedEvidence = [
    'dispatch-error|surface=spot|kind=request|reason=no_handler|action=fail_caller|packet=MissingSpotReq',
    'dispatch-error|surface=spot|kind=send|reason=no_handler|action=drop|packet=MissingSpotMsg'
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-E1 missing handler evidence mismatch.'
  );

  console.log('scenario SM-E1 passed');
}
