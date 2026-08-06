// SM-B0: Explicit type create와 existing-only Find를 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMB0(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-B0');
}
