// RL-F8: Manual topology에서는 Host Relocate를 시작하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF8(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F8');
}
