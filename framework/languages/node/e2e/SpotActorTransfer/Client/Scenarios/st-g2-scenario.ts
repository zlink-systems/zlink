// ST-G2: User Spot aggregate capacity를 all-or-none으로 적용한다 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG2(): Promise<void> {
  await runSpotActorCoverage('ST-G2');
}
