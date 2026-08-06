// SF-C4: 여러 service role을 가진 host를 한 lifecycle로 정리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFC4(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-C4');
}
