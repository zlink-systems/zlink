// SF-F4: ObjectGeneration과 owner replacement를 public ref로 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF4(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F4');
}
