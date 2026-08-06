// RL-F13: 많은 large-state units의 relocation을 bounded terminal로 끝낸다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runResilienceCoverage } from '../Support/coverage-scenarios';

export async function runRLF13(options: ClientOptions): Promise<void> {
  await runResilienceCoverage(options, 'RL-F13');
}
