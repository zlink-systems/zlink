// SF-F3: Relocation Store 장애는 새 relocation만 막는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF3(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F3');
}
