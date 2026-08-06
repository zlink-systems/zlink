// SF-G2: Unlimited population과 activation concurrency를 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFG2(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-G2');
}
