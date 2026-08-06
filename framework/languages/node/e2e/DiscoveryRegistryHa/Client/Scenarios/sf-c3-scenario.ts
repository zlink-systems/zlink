// SF-C3: 이전 owner lifecycle이 replacement를 바꾸지 못한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFC3(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-C3');
}
