// RL-F4: Client role이 없는 ClientServer process는 outbound 호출하지 못한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF4(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F4');
}
