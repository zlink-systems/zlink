// SF-F10: 많은 accepted requests와 relocation completion을 함께 처리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF10(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F10');
}
