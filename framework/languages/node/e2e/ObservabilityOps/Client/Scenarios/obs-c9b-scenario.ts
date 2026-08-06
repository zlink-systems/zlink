// OBS-C9B: Manual topology는 Relocate를 preflight에서 막는다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC9B(): Promise<void> {
  await runObservabilityCoverage('OBS-C9B');
}
