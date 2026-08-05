// SM-C4: Local Spot이 없는 MeshNode가 Logical Multicast를 publish한다 시나리오를 검증한다.
import type {
  CreateSpotRes,
  CreateSpotReq,
  SpotPublishObserveRes,
  SpotPublishRes,
  SpotPublishReq
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmC4(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-c4-${Date.now()}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(
    created.spotId === spotId && created.nodeRid === 'play-a',
    'SM-C4 publish spot was not created on play-a.'
  );

  const unsubscribedSpotId = `spot-sm-c4-unsubscribed-${Date.now()}`;
  const unsubscribed = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create-alternate', {
    spotId: unsubscribedSpotId
  } satisfies CreateSpotReq);
  ensure(
    unsubscribed.spotId === unsubscribedSpotId && unsubscribed.nodeRid === 'play-a',
    'SM-C4 unsubscribed spot was not created on play-a.'
  );

  const readyMarker = `sm-c4-ready-${Date.now()}`;
  await postJson<SpotPublishRes>(options.playAUrl, '/spot/publish/local', {
    spotId,
    marker: readyMarker
  } satisfies SpotPublishReq);
  await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [`spot-msg|rid=play-a|spot=${spotId}|marker=${readyMarker}`]
  });

  const marker = 'sm-c4-publish';
  const waitTask = postJson<SpotPublishObserveRes>(options.playAUrl, '/spot/publish/wait', {
    spotId,
    marker
  } satisfies SpotPublishReq);
  const publish = await postJson<SpotPublishRes>(options.gatewayUrl, '/spot/publish', {
    spotId,
    marker
  } satisfies SpotPublishReq);
  const observe = await waitTask;

  ensure(publish.operation === 'spot.sm-c4-publish', 'SM-C4 publish operation mismatch.');
  ensure(publish.publisherRid === 'gateway', 'SM-C4 publisher was not the publish-only gateway.');
  ensure(publish.spotId === spotId, 'SM-C4 publish target spot mismatch.');
  ensure(observe.operation === 'spot.sm-c4-observe', 'SM-C4 observe operation mismatch.');
  ensure(observe.received, 'SM-C4 publish-only gateway event was not received.');
  ensure(
    publish.evidence.some((line) => line.includes(`spot-publish|rid=gateway|spot=${spotId}|marker=${marker}`)),
    'SM-C4 gateway evidence did not include publish marker.'
  );
  const subscribedDeliveries = observe.evidence.filter(
    (line) => line.includes(`spot-msg|rid=play-a|spot=${spotId}|marker=${marker}`)
  );
  ensure(subscribedDeliveries.length === 1, 'SM-C4 subscribed Spot did not receive the publish exactly once.');
  ensure(
    observe.evidence.every((line) => !line.includes(`spot-msg|rid=play-a|spot=${unsubscribedSpotId}|marker=${marker}`)),
    'SM-C4 unsubscribed spot received publish event.'
  );

  console.log('scenario SM-C4 passed');
}
