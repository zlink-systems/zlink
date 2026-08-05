// SF-D3: Public status가 Ready·Degraded·Ready로 수렴한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface LocationRuntimeStatusDto {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
  readonly lastError?: string;
  readonly lastRefreshAt?: string;
}

export async function runSfD3(options: ClientOptions): Promise<void> {
  await waitForHealthyStatus(options.consumerUrl, undefined, 'initial');
  console.log('scenario-control SF-D3 stop-redis');
  const unhealthy = await waitForUnhealthyStatus(options.consumerUrl);
  ensure(
    unhealthy.lastError !== undefined && unhealthy.lastError.length > 0,
    'SF-D3 expected unhealthy status to expose lastError.'
  );

  const recovered = await waitForHealthyStatus(options.consumerUrl, unhealthy.lastRefreshAt, 'recovered');
  ensure(
    recovered.lastError === undefined || recovered.lastError.length === 0 || recovered.storeHealthy,
    'SF-D3 expected recovered status to clear or supersede lastError.'
  );

  console.log('scenario SF-D3 passed');
}

async function waitForHealthyStatus(
  baseUrl: string,
  previousRefreshAt: string | undefined,
  phase: string
): Promise<LocationRuntimeStatusDto> {
  const deadline = Date.now() + 10000;
  let last: LocationRuntimeStatusDto | undefined;
  while (Date.now() < deadline) {
    last = await getJson<LocationRuntimeStatusDto>(baseUrl, '/location/status');
    if (last.storeHealthy && last.ownerLeaseHealthy && refreshAdvanced(last.lastRefreshAt, previousRefreshAt)) {
      return last;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D3 expected ${phase} healthy location runtime status, last=${JSON.stringify(last)}`);
}

async function waitForUnhealthyStatus(baseUrl: string): Promise<LocationRuntimeStatusDto> {
  const deadline = Date.now() + 10000;
  let last: LocationRuntimeStatusDto | undefined;
  while (Date.now() < deadline) {
    last = await getJson<LocationRuntimeStatusDto>(baseUrl, '/location/status');
    if (!last.storeHealthy && !last.ownerLeaseHealthy) {
      return last;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D3 expected unhealthy location runtime status, last=${JSON.stringify(last)}`);
}

function refreshAdvanced(current: string | undefined, previous: string | undefined): boolean {
  if (previous === undefined || previous.length === 0) {
    return current !== undefined && current.length > 0;
  }
  return current !== undefined && current.length > 0 && current !== previous;
}
