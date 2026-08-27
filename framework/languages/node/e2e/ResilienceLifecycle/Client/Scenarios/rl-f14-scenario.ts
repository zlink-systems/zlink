// RL-F14: RelayReady 승인 전 명시 abort 뒤 source queue 순서를 복원한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF14(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F14');
}
