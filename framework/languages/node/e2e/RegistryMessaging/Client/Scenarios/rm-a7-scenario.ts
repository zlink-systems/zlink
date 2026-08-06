// RM-A7: Global Actor·Spot identity 충돌 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runRegistryCoverage } from '../Support/coverage-scenarios';

export async function runRMA7(options: ClientOptions): Promise<void> {
  await runRegistryCoverage(options, 'RM-A7');
}
