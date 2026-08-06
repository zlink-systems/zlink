// ST-H4A: Completion and timeout race 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTH4A(): Promise<void> {
  await runSpotActorCoverage('ST-H4A');
}
