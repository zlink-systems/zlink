// RL-F9: Preflight timeout과 post-seal deadline을 구분한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF9(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F9');
}
