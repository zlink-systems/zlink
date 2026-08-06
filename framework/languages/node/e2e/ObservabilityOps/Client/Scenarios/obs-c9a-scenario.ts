// OBS-C9A: Automatic topology는 target ready 뒤 relocation을 시작한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC9A(): Promise<void> {
  await runObservabilityCoverage('OBS-C9A');
}
