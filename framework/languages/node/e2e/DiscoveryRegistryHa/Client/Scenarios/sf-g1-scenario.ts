// SF-G1: Actor·Spot·stable type limit을 atomic하게 적용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFG1(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-G1');
}
