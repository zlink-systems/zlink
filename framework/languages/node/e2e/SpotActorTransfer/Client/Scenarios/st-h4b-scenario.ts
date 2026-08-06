// ST-H4B: Yield and reply terminal 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTH4B(): Promise<void> {
  await runSpotActorCoverage('ST-H4B');
}
