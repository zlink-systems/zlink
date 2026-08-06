// RL-E4: Connection loss 경쟁에서도 terminal 하나를 만든다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLE4(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-E4');
}
