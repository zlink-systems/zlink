// SF-C5: Public operational query를 bounded page로 읽는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFC5(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-C5');
}
