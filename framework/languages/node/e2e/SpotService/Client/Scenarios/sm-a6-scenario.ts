// SM-A6: User Spot initialize와 close lifecycle을 실행한다 시나리오를 검증한다.
import type { CloseSpotRes, CloseSpotReq, CreateSpotRes, CreateSpotReq, EvidenceWaitReq } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA6(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a6-${Date.now().toString(36)}`;
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-A6 lifecycle spot was not created.');
  ensure(created.nodeRid === 'play-a', 'SM-A6 lifecycle spot was created on the wrong node.');

  const closed = await postJson<CloseSpotRes>(options.playAUrl, '/spot/close', {
    spotId
  } satisfies CloseSpotReq);
  ensure(closed.closed, 'SM-A6 did not close the lifecycle spot.');

  const expected = [
    `spot-initialize|rid=play-a|spot=${spotId}`,
    `spot-closing|rid=play-a|spot=${spotId}`
  ];
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: expected,
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    expected.every((marker) => evidence.some((line) => line.includes(marker))),
    'SM-A6 lifecycle evidence mismatch.'
  );

  console.log('scenario SM-A6 passed');
}
