// RL-E2: Half-open connection을 application traffic과 독립적으로 판정한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLE2(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-E2');
}
