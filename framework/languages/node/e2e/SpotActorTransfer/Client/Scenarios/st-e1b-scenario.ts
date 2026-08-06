// ST-E1B: Relocation mode별 binding route 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTE1B(): Promise<void> {
  await runSpotActorCoverage('ST-E1B');
}
