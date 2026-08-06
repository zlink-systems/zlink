// ST-G1: Yielded continuation barrier 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG1(): Promise<void> {
  await runSpotActorCoverage('ST-G1');
}
