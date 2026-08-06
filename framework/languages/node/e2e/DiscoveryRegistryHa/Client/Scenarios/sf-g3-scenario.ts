// SF-G3: User Spot aggregate capacity를 all-or-none으로 적용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFG3(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-G3');
}
