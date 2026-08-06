// RL-F11: Ready relocation units를 느린 units보다 먼저 완료한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF11(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F11');
}
