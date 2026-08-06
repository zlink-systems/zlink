// RL-F2: Rebind 뒤 이전 Session message를 새 binding에 적용하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF2(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F2');
}
