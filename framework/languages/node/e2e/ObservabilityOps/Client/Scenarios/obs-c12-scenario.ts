// OBS-C12: Relocate waiter와 Shutdown 경쟁을 구분한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC12(): Promise<void> {
  await runObservabilityCoverage('OBS-C12');
}
