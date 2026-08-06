// RL-F1: Capacity가 바뀐 preflight에서 source를 보존한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF1(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F1');
}
