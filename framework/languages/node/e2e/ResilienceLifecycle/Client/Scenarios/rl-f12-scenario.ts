// RL-F12: User Spot queue와 timer를 relocation 뒤 복원한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF12(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F12');
}
