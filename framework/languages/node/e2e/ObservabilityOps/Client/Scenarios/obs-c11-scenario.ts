// OBS-C11: Concurrent Relocate option 충돌을 처리한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC11(): Promise<void> {
  await runObservabilityCoverage('OBS-C11');
}
