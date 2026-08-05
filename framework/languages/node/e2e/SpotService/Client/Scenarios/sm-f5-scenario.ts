// SM-F5: Spot close가 MeshNode Channel을 종료하지 않는다 시나리오를 검증한다.
import type {
  ChannelRouteRes,
  ChannelRouteReq,
  CloseSpotRes,
  CloseSpotReq,
  CreateSpotRes,
  CreateSpotReq,
  EvidenceWaitReq,
  SpotMissingTargetRes,
  SpotMissingTargetReq,
  SpotMixedRouteRes,
  SpotMixedRouteReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmF5(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-f5-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-F5 did not create the requested spot.');
  const ownerUrl = created.nodeRid === 'play-a'
    ? options.playAUrl
    : created.nodeRid === 'play-b'
      ? options.playBUrl
      : undefined;
  ensure(ownerUrl !== undefined, `SM-F5 created spot on unexpected node '${created.nodeRid}'.`);
  const nonOwnerUrl = created.nodeRid === 'play-a' ? options.playBUrl : options.playAUrl;

  const mixed = await postJson<SpotMixedRouteRes>(nonOwnerUrl, '/spot/mixed-route/request', {
    spotId,
    channelValue: 'sm-f5-before-close',
    delta: 13
  } satisfies SpotMixedRouteReq);
  ensure(mixed.channelReply === 'echo-sm-f5-before-close', 'SM-F5 pre-close channel reply mismatch.');
  ensure(mixed.spotValue === 13, 'SM-F5 pre-close spot route reply mismatch.');

  const closed = await postJson<CloseSpotRes>(ownerUrl, '/spot/close', {
    spotId
  } satisfies CloseSpotReq);
  ensure(closed.closed, 'SM-F5 did not close the spot.');

  const closedSpot = await postJson<SpotMissingTargetRes>(nonOwnerUrl, '/spot/missing-target/request', {
    spotId
  } satisfies SpotMissingTargetReq);
  ensure(closedSpot.failed, 'SM-F5 closed spot route did not fail.');

  const channelAfterClose = await postJson<ChannelRouteRes>(nonOwnerUrl, '/channel/route/request', {
    value: 'sm-f5-after-close'
  } satisfies ChannelRouteReq);
  ensure(channelAfterClose.value === 'echo-sm-f5-after-close', 'SM-F5 channel reply after spot close mismatch.');

  const ownerEvidence = [
    `spot-state-request|rid=${created.nodeRid}|spot=${spotId}|value=13`,
    `spot-closing|rid=${created.nodeRid}|spot=${spotId}`
  ];
  const observedOwnerEvidence = await postJson<string[]>(ownerUrl, '/evidence/wait', {
    containsAll: ownerEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    ownerEvidence.every((expected) => observedOwnerEvidence.some((line) => line.includes(expected))),
    'SM-F5 owner Spot lifecycle evidence mismatch.'
  );
  const channelEvidence = [
    'channel-echo|value=sm-f5-before-close',
    'channel-echo|value=sm-f5-after-close'
  ];
  const observedChannelEvidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: channelEvidence,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    channelEvidence.every((expected) => observedChannelEvidence.some((line) => line.includes(expected))),
    'SM-F5 ChannelName lifecycle independence evidence mismatch.'
  );

  console.log('scenario SM-F5 passed');
}
