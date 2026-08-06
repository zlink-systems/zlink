// OBS-C6: Rolling update로 exact 새 version에 이동한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC6(): Promise<void> {
  await runObservabilityCoverage('OBS-C6');
}
