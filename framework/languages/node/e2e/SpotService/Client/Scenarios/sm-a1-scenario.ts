// SM-A1: Entry Spot request로 User Spot을 만든다 시나리오를 검증한다.
import type { CreateSpotRes, CreateSpotReq, EvidenceWaitReq } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA1(options: ClientOptions): Promise<void> {
  const spotId = 'sm-a1-user';
  const created = await postJson<CreateSpotRes>(options.playAUrl, '/spot/create', {
    spotId
  } satisfies CreateSpotReq);
  ensure(created.spotId === spotId, 'SM-A1 did not create the requested spot.');
  ensure(created.nodeRid === 'play-a', 'SM-A1 created spot on the wrong node.');

  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [`create-spot|rid=play-a|spot=${spotId}`],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    evidence.some((line) => line.includes(`create-spot|rid=play-a|spot=${spotId}`)),
    'SM-A1 create-spot evidence missing.'
  );

  console.log('scenario SM-A1 passed');
}
