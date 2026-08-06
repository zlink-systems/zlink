// RL-E5: Store 장애와 transport liveness를 독립적으로 처리한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLE5(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-E5');
}
