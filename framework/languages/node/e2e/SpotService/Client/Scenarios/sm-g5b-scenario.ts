// SM-G5B: Capacity가 없는 high-weight node를 신규 placement에서 제외한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMG5B(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-G5B');
}
