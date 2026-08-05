// SM-F4: Missing Spot과 stale SpotRef를 구분한다 시나리오를 검증한다.
import type {
  ChannelRouteRes,
  ChannelRouteReq,
  CloseSpotExactRes,
  CloseSpotExactReq,
  CloseSpotRes,
  CloseSpotReq,
  CreateSpotRes,
  CreateSpotReq,
  NodeRouteRes,
  NodeRouteReq,
  SpotMissingTargetMsgRes,
  SpotMissingTargetMsgReq,
  SpotMissingTargetRes,
  SpotMissingTargetReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmF4(options: ClientOptions): Promise<void> {
  const missingSpotId = `missing-spot-sm-f4-${Date.now()}`;
  const missingTarget = await postJson<SpotMissingTargetRes>(options.playAUrl, '/spot/missing-target/request', {
    spotId: missingSpotId
  } satisfies SpotMissingTargetReq);
  ensure(missingTarget.failed, 'SM-F4 missing target request did not fail.');
  ensure(
    missingTarget.errorKind === 'requestTargetNotFound',
    'SM-F4 missing request did not report requestTargetNotFound.'
  );

  const missingSend = await postJson<SpotMissingTargetMsgRes>(
    options.playAUrl,
    '/spot/missing-target/command',
    { spotId: missingSpotId, marker: 'sm-f4-missing-send' } satisfies SpotMissingTargetMsgReq
  );
  ensure(missingSend.failed === true && !missingSend.sent, 'SM-F4 missing target send did not fail.');
  ensure(
    missingSend.errorKind === 'spotRouteNotFound',
    'SM-F4 missing send did not report spotRouteNotFound.'
  );
  ensure(
    !missingSend.evidence.some((entry) => entry.includes(`spot=${missingSpotId}`)),
    'SM-F4 missing target unexpectedly activated a Spot.'
  );

  const spotId = `spot-sm-f4-${Date.now()}`;
  const first = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    first.nodeRid !== undefined
      && first.objectGeneration !== undefined
      && first.meshName !== undefined,
    'SM-F4 first SpotRef was incomplete.'
  );
  const closed = await postJson<CloseSpotRes>(options.playAUrl, '/spot/close', {
    spotId
  } satisfies CloseSpotReq);
  ensure(closed.closed, 'SM-F4 first incarnation did not close.');

  const second = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    second.nodeRid !== undefined
      && second.objectGeneration !== undefined
      && second.meshName !== undefined,
    'SM-F4 second SpotRef was incomplete.'
  );
  ensure(
    second.objectGeneration !== first.objectGeneration,
    'SM-F4 recreated Spot did not advance object generation.'
  );

  const staleClose = await postJson<CloseSpotExactRes>(options.playAUrl, '/spot/close-exact', {
    spotId,
    objectGeneration: first.objectGeneration,
    meshName: first.meshName,
    nodeRid: first.nodeRid
  } satisfies CloseSpotExactReq);
  ensure(
    staleClose.staleGeneration
      && !staleClose.closed
      && staleClose.errorKind === 'spotGenerationStale',
    'SM-F4 stale SpotRef did not report spotGenerationStale.'
  );

  const channel = await postJson<ChannelRouteRes>(options.playAUrl, '/channel/route/request', {
    value: 'sm-f4-channel'
  } satisfies ChannelRouteReq);
  ensure(channel.value === 'echo-sm-f4-channel', 'SM-F4 ChannelName route regressed.');

  const node = await postJson<NodeRouteRes>(options.playAUrl, '/node/route/request', {
    nodeRid: second.nodeRid,
    value: 'sm-f4-node'
  } satisfies NodeRouteReq);
  ensure(node.value === 'echo-sm-f4-node', 'SM-F4 RID direct route regressed.');

  const currentClose = await postJson<CloseSpotExactRes>(options.playAUrl, '/spot/close-exact', {
    spotId,
    objectGeneration: second.objectGeneration,
    meshName: second.meshName,
    nodeRid: second.nodeRid
  } satisfies CloseSpotExactReq);
  ensure(
    currentClose.closed && !currentClose.staleGeneration,
    'SM-F4 stale close affected the current Spot incarnation.'
  );

  console.log('scenario SM-F4 passed');
}
