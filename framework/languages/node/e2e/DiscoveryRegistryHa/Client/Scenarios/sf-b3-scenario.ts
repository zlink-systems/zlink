// SF-B3: Discovery grace가 stateful owner lease를 연장하지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

const INSTANCE_TIMER_PERIOD_MS = 100;

interface ObjectReply {
  readonly spotId: string;
  readonly operationId: string;
  readonly payload: string;
}

interface LocationStatus {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
}

type Evidence = readonly string[];

export async function runSFB3(options: ClientOptions): Promise<void> {
  const spotId = 'sf-b3-instance';
  const baseline = await postJson<ObjectReply>(options.consumerUrl, '/object/request', {
    spotId,
    operationId: 'sf-b3-baseline',
    payload: 'sf-b3-baseline'
  });
  ensure(baseline.payload === 'sf-b3-baseline', 'SF-B3 baseline Instance Spot request failed.');
  let before = await getJson<Evidence>(`${options.providerAUrl}/evidence`);
  for (let attempt = 0; attempt < 50 && !before.some((entry) => entry.includes('object-timer|')); attempt += 1) {
    await new Promise((resolve) => setTimeout(resolve, 100));
    before = await getJson<Evidence>(`${options.providerAUrl}/evidence`);
  }
  ensure(before.some((entry) => entry.includes('object-timer|')), 'SF-B3 timer did not produce baseline evidence.');
  console.log('scenario-control SF-B3 stop-redis');
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const status = await getJson<LocationStatus>(`${options.consumerUrl}/location/status`);
    if (!status.storeHealthy && !status.ownerLeaseHealthy) break;
    await new Promise((resolve) => setTimeout(resolve, 100));
    if (attempt === 99) throw new Error('SF-B3 Location Store did not enter unhealthy state.');
  }
  for (let attempt = 0; attempt < 100; attempt += 1) {
    const providerStatus = await getJson<LocationStatus>(`${options.providerAUrl}/location/status`);
    if (!providerStatus.ownerLeaseHealthy) break;
    await new Promise((resolve) => setTimeout(resolve, 100));
    if (attempt === 99) throw new Error('SF-B3 provider owner lease did not expire.');
  }
  const expired = await getJson<Evidence>(`${options.providerAUrl}/evidence`);
  const expiredTimers = expired.filter((entry) => entry.includes('object-timer|')).length;
  await new Promise((resolve) => setTimeout(resolve, INSTANCE_TIMER_PERIOD_MS * 2));
  let failed = false;
  try {
    await postJson<ObjectReply>(options.consumerUrl, '/object/request', {
      spotId,
      operationId: 'sf-b3-after-lease',
      payload: 'sf-b3-after-lease'
    });
  } catch {
    failed = true;
  }
  ensure(failed, 'SF-B3 stateful request succeeded after owner lease expiry.');
  const after = await getJson<Evidence>(`${options.providerAUrl}/evidence`);
  const afterTimers = after.filter((entry) => entry.includes('object-timer|')).length;
  ensure(afterTimers === expiredTimers, `SF-B3 timer evidence increased after lease expiry (${expiredTimers} -> ${afterTimers}).`);
  console.log('scenario SF-B3 passed');
}
