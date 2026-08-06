// SM-G5A: Placement weight 100:300 비율을 충분한 표본으로 확인한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMG5A(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-G5A');
}
