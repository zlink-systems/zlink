// OBS-A5: 실행 중 tracing level 변경을 적용한다 시나리오를 검증한다.
import { runObservabilityCoverage } from '../Support/coverage-scenarios.js';

export async function runOBSA5(): Promise<void> {
  await runObservabilityCoverage('OBS-A5');
}
