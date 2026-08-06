// RL-F10: Entry Actor와 SpotWide aggregate를 Host Relocate한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF10(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F10');
}
