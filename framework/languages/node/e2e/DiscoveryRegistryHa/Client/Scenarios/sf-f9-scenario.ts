// SF-F9: Old lifecycle cleanup이 replacement service roles를 제거하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF9(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F9');
}
