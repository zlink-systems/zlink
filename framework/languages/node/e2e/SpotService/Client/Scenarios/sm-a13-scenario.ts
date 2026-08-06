// SM-A13: SpotId UTF-8 길이와 exact equality를 지킨다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMA13(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-A13');
}
