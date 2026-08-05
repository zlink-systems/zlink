// SF-B1: Store 장애 중 기존 connection을 유지한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface LocationRuntimeStatusDto {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
  readonly lastError?: string;
}

export async function runSfB1(options: ClientOptions): Promise<void> {
  const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value: 'sf-b1' });
  ensure(reply.value === 'profile:sf-b1', 'SF-B1 reply value mismatch during store outage.');
  ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', 'SF-B1 provider rid mismatch during store outage.');

  const status = await waitForStoreUnhealthy(options.consumerUrl);
  ensure(status.lastError !== undefined && status.lastError.length > 0, 'SF-B1 expected store outage status to include lastError.');

  const providerEvidence = [
    ...await getJson<string[]>(options.providerAUrl, '/evidence'),
    ...(options.providerBUrl === undefined ? [] : await getJson<string[]>(options.providerBUrl, '/evidence'))
  ];
  ensure(providerEvidence.some((entry) => entry.includes('value=sf-b1')), 'SF-B1 provider evidence missing.');
  console.log('scenario SF-B1 passed');
}

async function waitForStoreUnhealthy(baseUrl: string): Promise<LocationRuntimeStatusDto> {
  const deadline = Date.now() + 10000;
  let last: LocationRuntimeStatusDto | undefined;
  while (Date.now() < deadline) {
    last = await getJson<LocationRuntimeStatusDto>(baseUrl, '/location/status');
    if (!last.storeHealthy && !last.ownerLeaseHealthy) {
      return last;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-B1 expected unhealthy location store status, last=${JSON.stringify(last)}`);
}
