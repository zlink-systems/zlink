// ST-F3A: Late Session route update 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTF3A(): Promise<void> {
  await runSpotActorCoverage('ST-F3A');
}
