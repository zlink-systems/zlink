// OBS-C7: Planned maintenance는 같은 version target을 사용한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSC7(): Promise<void> {
  await runObservabilityCoverage('OBS-C7');
}
