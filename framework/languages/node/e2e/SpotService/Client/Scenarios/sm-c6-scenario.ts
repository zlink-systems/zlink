// SM-C6: Logical Multicast partial backpressure를 다른 target과 격리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMC6(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-C6');
}
