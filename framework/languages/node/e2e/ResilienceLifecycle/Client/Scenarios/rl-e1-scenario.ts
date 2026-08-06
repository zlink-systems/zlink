// RL-E1: Orderly disconnect를 peer deadline 전에 반영한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLE1(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-E1');
}
