// ST-G3: PerActor Spot relocation 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG3(): Promise<void> {
  await runSpotActorCoverage('ST-G3');
}
