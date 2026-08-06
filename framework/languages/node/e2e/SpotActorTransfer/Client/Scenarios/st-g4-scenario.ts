// ST-G4: ToActor message during Spot move 시나리오를 검증한다.
import { runSpotActorCoverage } from '../Support/coverage-scenarios';

export async function runSTG4(): Promise<void> {
  await runSpotActorCoverage('ST-G4');
}
