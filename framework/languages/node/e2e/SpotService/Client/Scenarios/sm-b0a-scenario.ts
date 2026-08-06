// SM-B0A: Actor creation accept와 reject를 operation별로 반환한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMB0A(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-B0A');
}
