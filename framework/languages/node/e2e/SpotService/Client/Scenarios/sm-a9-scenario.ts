// SM-A9: User Spot은 initialize 완료 뒤 Ready로 공개한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMA9(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-A9');
}
