// ST-G2: SpotWide durable backlog와 lazy job admission 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG2(): Promise<void> {
  await runSpotActorCoverage('ST-G2');
}
