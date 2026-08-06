// SM-A12: Automatic User Spot IDs가 concurrent creates에서 서로 다르다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMA12(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-A12');
}
