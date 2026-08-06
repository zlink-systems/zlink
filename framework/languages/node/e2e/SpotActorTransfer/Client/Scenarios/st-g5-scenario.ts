// ST-G5: Relocation interruption measurement 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG5(): Promise<void> {
  await runSpotActorCoverage('ST-G5');
}
