// SF-B3: Discovery grace가 stateful owner lease를 연장하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFB3(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-B3');
}
