// ST-E1C: Session location update retry 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTE1C(): Promise<void> {
  await runSpotActorCoverage('ST-E1C');
}
