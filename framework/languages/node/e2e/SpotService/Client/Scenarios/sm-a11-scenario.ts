// SM-A11: Entry Spot 예약 형식을 User·Instance ID로 거부한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { runSpotServiceCoverage } from '../Support/coverage-scenarios';

export async function runSMA11(options: ClientOptions): Promise<void> {
  await runSpotServiceCoverage(options, 'SM-A11');
}
