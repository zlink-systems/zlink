// RL-F5: Relocation 중 받은 messages를 target에서 순서대로 처리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF5(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F5');
}
