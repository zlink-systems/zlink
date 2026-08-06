// SF-F5: Creating owner crash 뒤 public request가 bounded recovery 결과를 얻는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF5(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F5');
}
