// SM-C5: Logical Multicast remote delivery를 subscriber evidence로 판정한다 시나리오를 검증한다.
import type {
  CreateSpotReq,
  CreateSpotRes,
  EvidenceWaitReq,
  SpotPublishReq,
  SpotPublishRes
} from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmC5(options: ClientOptions): Promise<void> {
  const suffix = Date.now();
  const sourceSpotId = `spot-sm-c5-source-${suffix}`;
  const targetSpotId = `spot-sm-c5-target-${suffix}`;
  const source = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId: sourceSpotId
  } satisfies CreateSpotReq);
  const target = await postJson<CreateSpotRes>(options.playBUrl, '/spot/create', {
    spotId: targetSpotId
  } satisfies CreateSpotReq);
  ensure(source.nodeRid === 'play-a' && source.spotId === sourceSpotId, 'SM-C5 source spot mismatch.');
  ensure(target.nodeRid === 'play-b' && target.spotId === targetSpotId, 'SM-C5 target spot mismatch.');

  await waitForCrossNodeSubscription(options, sourceSpotId, targetSpotId, `sm-c5-ready-${suffix}`);

  const marker = `sm-c5-cross-node-${suffix}`;
  const publish = await postJson<SpotPublishRes>(options.playAUrl, '/spot/publish/local', {
    spotId: sourceSpotId,
    marker
  } satisfies SpotPublishReq);
  ensure(publish.publisherRid === 'play-a', 'SM-C5 publisher node mismatch.');

  const expected = `spot-msg|rid=play-b|spot=${targetSpotId}|marker=${marker}`;
  const evidence = await postJson<string[]>(options.playBUrl, '/evidence/wait', {
    containsAll: [expected],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(evidence.some((line) => line.includes(expected)), 'SM-C5 cross-node SPOT publish was not received.');

  console.log('scenario SM-C5 passed');
}

async function waitForCrossNodeSubscription(
  options: ClientOptions,
  sourceSpotId: string,
  targetSpotId: string,
  marker: string
): Promise<void> {
  const expected = `spot-msg|rid=play-b|spot=${targetSpotId}|marker=${marker}`;
  const deadline = Date.now() + 30000;
  let last: unknown;
  while (Date.now() < deadline) {
    await postJson<SpotPublishRes>(options.playAUrl, '/spot/publish/local', {
      spotId: sourceSpotId,
      marker
    } satisfies SpotPublishReq);
    try {
      const evidence = await postJson<string[]>(options.playBUrl, '/evidence/wait', {
        containsAll: [expected],
        timeoutMilliseconds: 1000
      } satisfies EvidenceWaitReq);
      if (evidence.some((line) => line.includes(expected))) {
        return;
      }
    } catch (error) {
      last = error;
    }
  }
  throw new Error(`Timed out waiting for SM-C5 cross-node subscription readiness: ${String(last)}`);
}
