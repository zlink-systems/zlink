// SF-F2: 장기 relocation은 Store lease를 유지하고 실패 뒤 새 call을 허용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runDiscoveryCoverage } from '../Support/coverage-scenarios';

export async function runSFF2(options: ClientOptions): Promise<void> {
  await runDiscoveryCoverage(options, 'SF-F2');
}
