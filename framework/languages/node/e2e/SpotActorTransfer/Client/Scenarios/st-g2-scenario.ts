// ST-G2: User Spot aggregate capacity 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG2(): Promise<void> {
  await runSpotActorCoverage('ST-G2');
}
