// ST-G6: SpotWide application boundary 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG6(): Promise<void> {
  await runSpotActorCoverage('ST-G6');
}
