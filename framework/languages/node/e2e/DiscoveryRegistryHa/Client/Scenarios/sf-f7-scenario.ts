// SF-F7: Large state relocation은 public size limit 안에서 복원한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF7(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F7');
}
