// RL-E3: Reconnect 전 old reply가 새 request를 완료하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLE3(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-E3');
}
