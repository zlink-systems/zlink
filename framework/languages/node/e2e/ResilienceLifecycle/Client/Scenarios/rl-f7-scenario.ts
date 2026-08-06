// RL-F7: Relocated accepted request는 terminal 하나를 반환한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF7(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F7');
}
