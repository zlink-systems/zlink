// SM-C2: Spot handler에서 Channel request를 보낸다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotOutboundRouteRes,
  SpotOutboundRouteReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmC2(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-c2-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    created.spotId === spotId && created.nodeRid === 'play-a',
    'SM-C2 spot was not created on play-a.'
  );

  const outbound = await postJson<SpotOutboundRouteRes>(options.playAUrl, '/spot/outbound', {
    spotId,
    marker: 'sm-c2'
  } satisfies SpotOutboundRouteReq);
  ensure(outbound.accepted, 'SM-C2 outbound route was not accepted.');

  const negative = await postJson<SpotOutboundRouteRes>(options.playAUrl, '/spot/outbound-negative', {
    spotId,
    marker: 'sm-c2-missing'
  } satisfies SpotOutboundRouteReq);
  ensure(negative.accepted, 'SM-C2 negative outbound route was not accepted.');

  const expectedEvidence = [
    `spot-outbound|rid=play-a|spot=${spotId}|echo=echo-sm-c2|notify=notify-sm-c2`,
    `spot-msg|rid=play-a|spot=${spotId}|marker=sm-c2-publish`,
    `spot-outbound-negative|rid=play-a|spot=${spotId}|requestFailed=True`,
    'channel-echo|value=sm-c2',
    'channel-notify|marker=notify-sm-c2',
    'dispatch-error|surface=channel|kind=request|reason=no_handler|action=reply_error|packet=MissingChannelReq',
    'dispatch-error|surface=channel|kind=send|reason=no_handler|action=drop|packet=MissingChannelNotify'
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expectedEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expectedEvidence.every((expected) => evidence.some((line) => line.includes(expected))),
    'SM-C2 evidence mismatch.'
  );

  console.log('scenario SM-C2 passed');
}
