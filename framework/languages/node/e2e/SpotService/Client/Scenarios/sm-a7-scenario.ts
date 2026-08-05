// SM-A7: 같은 SpotId의 stable type 충돌을 거부한다 시나리오를 검증한다.
import type { EvidenceWaitReq, SpotTypeMismatchRes, SpotTypeMismatchReq } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

export async function runSmA7(options: ClientOptions): Promise<void> {
  const spotId = `spot-sm-a7-${Date.now().toString(36)}`;
  const mismatch = await postJson<SpotTypeMismatchRes>(options.playAUrl, '/spot/type-mismatch', {
    spotId
  } satisfies SpotTypeMismatchReq);
  ensure(mismatch.failed, 'SM-A7 expected SpotTypeMismatch.');
  ensure(mismatch.errorKind === 'SpotTypeMismatch', 'SM-A7 error kind mismatch.');

  const expected = `spot-type-mismatch|rid=play-a|spot=${mismatch.spotId}|kind=SpotTypeMismatch`;
  const evidence = await postJson<string[]>(options.playAUrl, '/evidence/wait', {
    containsAll: [expected],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  ensure(
    evidence.some((line) => line.includes(expected)),
    'SM-A7 evidence did not include SpotTypeMismatch.'
  );

  console.log('scenario SM-A7 passed');
}
