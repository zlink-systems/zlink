// ST-B1: PreserveState relocation 시나리오를 검증한다.
import { SpotActorTransferNames, runRemoteTransfer, unique } from '../Support/scenario-support';

export async function runStB1(): Promise<void> {
  await runRemoteTransfer(
    'ST-B1',
    unique('actor-handoff-gate-st-b1'),
    SpotActorTransferNames.actorTypeStateful,
    21,
    true,
    true
  );
}
