// SF-C2: 정상 Shutdown은 lease expiry를 기다리지 않는다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface PeerDto {
  readonly endpoint: string;
  readonly nodeRid?: string;
  readonly draining: boolean;
}

interface RelocationResult {
  readonly outcome: number;
  readonly reason: number;
}

export async function runSfC2(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-C2 requires the api-b HTTP endpoint.');
  const drainStartedAt = Date.now();
  const drain = postJsonWithin<RelocationResult>(options.providerBUrl, '/drain', {}, 35_000);

  await waitForDrainingPeer(options.consumerUrl, 'api-b');
  await waitForProviderReply(options.consumerUrl, 'api-a', 'sf-c2-draining', 20);

  for (let i = 0; i < 8; i++) {
    const value = `sf-c2-draining-${i}`;
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value });
    ensure(reply.value === `profile:${value}`, `SF-C2 draining request ${i} value mismatch.`);
    ensure(reply.providerRid === 'api-a', `SF-C2 draining request ${i} reached '${reply.providerRid}'.`);
  }

  const result = await drain;
  ensure(
    result.outcome === 0 && result.reason === 0,
    `SF-C2 retire ended as '${result.outcome}/${result.reason}'.`
  );
  ensure(Date.now() - drainStartedAt < 30_000, 'SF-C2 drain exceeded the 30 second deadline.');
  await waitForMissingPeer(options.consumerUrl, 'api-b', 3_000);

  for (let i = 0; i < 4; i++) {
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value: `sf-c2-after-${i}` });
    ensure(reply.value === `profile:sf-c2-after-${i}`, `SF-C2 follow-up request ${i} value mismatch.`);
    ensure(reply.providerRid === 'api-a', `SF-C2 follow-up request ${i} was served by '${reply.providerRid}'.`);
  }

  const evidence = await getJson<string[]>(options.providerAUrl, '/evidence');
  ensure(evidence.some((entry) => entry.includes('value=sf-c2-after-')), 'SF-C2 api-a evidence missing.');
  console.log('scenario SF-C2 passed');
}

async function waitForDrainingPeer(baseUrl: string, rid: string): Promise<void> {
  const deadline = Date.now() + 3_000;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (last.some((peer) => peer.nodeRid === rid && peer.draining)) return;
    await new Promise((resolve) => setTimeout(resolve, 20));
  }
  throw new Error(`SF-C2 expected peer ${rid} to publish Draining=true, last=${JSON.stringify(last)}`);
}

async function waitForMissingPeer(baseUrl: string, rid: string, timeoutMs: number): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (!last.some((peer) => peer.nodeRid === rid)) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-C2 expected peer ${rid} to be absent after graceful shutdown, last=${JSON.stringify(last)}`);
}

async function waitForProviderReply(baseUrl: string, rid: string, prefix: string, required = 3): Promise<void> {
  const deadline = Date.now() + 10000;
  let index = 0;
  let consecutive = 0;
  while (Date.now() < deadline) {
    const value = `${prefix}-${index++}`;
    try {
      const reply = await postJson<ProfileRes>(baseUrl, '/profile/request', { value });
      ensure(reply.value === `profile:${value}`, `SF-C2 reply value mismatch for ${value}.`);
      if (reply.providerRid === rid) {
        consecutive += 1;
        if (consecutive >= required) {
          return;
        }
      } else {
        consecutive = 0;
      }
    } catch {
      // Wait for the routing table to converge after graceful provider shutdown.
      consecutive = 0;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-C2 expected request routing to reach ${rid}.`);
}
