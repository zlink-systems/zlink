// SF-D1: 짧은 장애는 기존 connection을 불필요하게 바꾸지 않는다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface LocationRuntimeStatusDto {
  readonly storeHealthy: boolean;
  readonly ownerLeaseHealthy: boolean;
}

interface PeerDto {
  readonly endpoint: string;
}

export async function runSfD1(options: ClientOptions): Promise<void> {
  const traffic = driveRequests(options.consumerUrl, 6000);
  console.log('scenario-control SF-D1 stop-redis');
  await delay(1500);
  console.log('scenario-control SF-D1 restart-redis');
  await traffic;

  await waitForHealthyStatus(options.consumerUrl);
  await waitForLiveProviders(options.consumerUrl);

  console.log('scenario SF-D1 passed');
}

async function delay(milliseconds: number): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function driveRequests(baseUrl: string, windowMs: number): Promise<void> {
  const deadline = Date.now() + windowMs;
  let count = 0;
  while (Date.now() < deadline) {
    const value = `sf-d1-${count++}`;
    let reply: ProfileRes;
    try {
      reply = await postJson<ProfileRes>(baseUrl, '/profile/request', { value });
    } catch (error) {
      const [route, location, peers] = await Promise.all([
        getJson<unknown>(baseUrl, '/route/status').catch(() => undefined),
        getJson<unknown>(baseUrl, '/location/status').catch(() => undefined),
        getJson<unknown>(baseUrl, '/location/peers').catch(() => undefined)
      ]);
      throw new Error(
        `SF-D1 request '${value}' failed: ${errorMessage(error)}; `
        + `route=${JSON.stringify(route)}; location=${JSON.stringify(location)}; `
        + `peers=${JSON.stringify(peers)}`
      );
    }
    ensure(reply.value.startsWith('profile:sf-d1-'), 'SF-D1 reply value mismatch during short outage recovery.');
    ensure(reply.providerRid === 'api-a' || reply.providerRid === 'api-b', 'SF-D1 provider rid mismatch during short outage recovery.');
    await new Promise((resolve) => setTimeout(resolve, 150));
  }
  ensure(count > 0, 'SF-D1 request window produced no traffic.');
}

function errorMessage(error: unknown): string {
  return error instanceof Error ? error.message : String(error);
}

async function waitForHealthyStatus(baseUrl: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: LocationRuntimeStatusDto | undefined;
  while (Date.now() < deadline) {
    last = await getJson<LocationRuntimeStatusDto>(baseUrl, '/location/status');
    if (last.storeHealthy && last.ownerLeaseHealthy) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D1 expected healthy location runtime after recovery, last=${JSON.stringify(last)}`);
}

async function waitForLiveProviders(baseUrl: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (last.length === 2) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D1 expected both provider rows live after recovery, last=${JSON.stringify(last)}`);
}
