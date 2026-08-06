// RL-F3: Cross-language terminal failure를 같게 해석한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF3(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F3');
}
