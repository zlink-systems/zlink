// SF-F11: Waiter 종료와 response loss 뒤 payload 값을 보존한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF11(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F11');
}
