// SF-F1: Cross-language object 위치와 state를 해석한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF1(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F1');
}
