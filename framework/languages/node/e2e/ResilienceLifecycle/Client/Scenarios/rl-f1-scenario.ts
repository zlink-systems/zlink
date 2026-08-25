// RL-F1: RelayReady 승인 전 capacity·availability 실패에서 source를 보존한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF1(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F1');
}
