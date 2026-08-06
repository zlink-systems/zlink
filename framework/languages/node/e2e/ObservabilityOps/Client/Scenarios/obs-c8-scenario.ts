// OBS-C8: Shutdown deadline에서 bounded forced teardown을 수행한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC8(): Promise<void> {
  await runObservabilityCoverage('OBS-C8');
}
