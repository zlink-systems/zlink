// SM-B10: Object role과 Location Store prerequisite를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMB10(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-B10');
}
