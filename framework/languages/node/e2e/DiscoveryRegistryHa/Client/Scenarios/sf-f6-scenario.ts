// SF-F6: Operational query 중 concurrent 변경을 다음 page cycle에 반영한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF6(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F6');
}
