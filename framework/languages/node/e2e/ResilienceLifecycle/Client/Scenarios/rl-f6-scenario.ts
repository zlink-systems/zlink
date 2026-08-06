// RL-F6: Runtime mutable update와 invalid mutation을 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF6(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F6');
}
