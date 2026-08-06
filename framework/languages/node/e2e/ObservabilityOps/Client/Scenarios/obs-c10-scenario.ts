// OBS-C10: Relocation mode가 정한 exact version만 선택한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC10(): Promise<void> {
  await runObservabilityCoverage('OBS-C10');
}
