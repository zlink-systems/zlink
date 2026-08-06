// SM-A10: Entry Spot ID는 MeshNode RID와 독립된 lifecycle identity다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMA10(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-A10');
}
