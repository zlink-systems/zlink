// SF-F8: Target owner lease가 만료되면 source를 유지한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF8(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F8');
}
