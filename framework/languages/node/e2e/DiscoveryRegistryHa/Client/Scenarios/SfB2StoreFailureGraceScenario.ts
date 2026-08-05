// SF-B2: Failure grace를 넘겨도 기존 connection과 신규 discovery를 구분한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface LocationRuntimeStatusDto {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
  readonly lastError?: string;
}

export async function runSfB2(options: ClientOptions): Promise<void> {
  const unhealthy = await waitForStoreUnhealthy(options.consumerUrl);
  ensure(
    unhealthy.lastError !== undefined && unhealthy.lastError.length > 0,
    'SF-B2 expected store outage status to include lastError.'
  );

  const replies = await driveRequestsPastGrace(options.consumerUrl, 7500);
  ensure(replies.length >= 4, `SF-B2 expected repeated successful traffic after grace, actual=${replies.length}.`);
  ensure(
    replies.every((reply) => reply.providerRid === 'api-a'),
    'SF-B2 connected to api-b while the store was unavailable.'
  );
  const apiBEvidence = await getJson<string[]>(options.providerBUrl, '/evidence');
  ensure(
    !apiBEvidence.some((entry) => entry.includes('value=sf-b2-')),
    'SF-B2 api-b evidence shows a new connection during the store outage.'
  );

  console.log('scenario SF-B2 passed');
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
  throw new Error(`SF-B2 expected unhealthy location store status, last=${JSON.stringify(last)}`);
}

async function driveRequestsPastGrace(baseUrl: string, windowMs: number): Promise<ProfileRes[]> {
  const deadline = Date.now() + windowMs;
  const replies: ProfileRes[] = [];
  let index = 0;
  while (Date.now() < deadline) {
    const value = `sf-b2-${index++}`;
    const reply = await postJson<ProfileRes>(baseUrl, '/profile/request', { value });
    ensure(reply.value === `profile:${value}`, `SF-B2 reply value mismatch for ${value}.`);
    replies.push(reply);
    await new Promise((resolve) => setTimeout(resolve, 300));
  }
  return replies;
}
