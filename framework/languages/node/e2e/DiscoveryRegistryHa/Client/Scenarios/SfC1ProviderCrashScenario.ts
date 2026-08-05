// SF-C1: Crash한 provider를 lease 만료 뒤 제외한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface PeerDto {
  readonly endpoint: string;
  readonly nodeRid?: string;
}

export async function runSfC1(options: ClientOptions): Promise<void> {
  const peers = await waitForSingleLivePeer(options.consumerUrl);
  ensure(peers.some((peer) => peer.nodeRid === 'api-a'), 'SF-C1 expected api-a to remain live.');

  await waitForProviderReply(options.consumerUrl, 'api-a', 'sf-c1-stable-after-crash');

  const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value: 'sf-c1-after-crash' });
  ensure(reply.value === 'profile:sf-c1-after-crash', 'SF-C1 reply value mismatch after provider crash.');
  ensure(reply.providerRid === 'api-a', 'SF-C1 expected follow-up request to route only to api-a.');

  const evidence = await getJson<string[]>(options.providerAUrl, '/evidence');
  ensure(evidence.some((entry) => entry.includes('value=sf-c1-after-crash')), 'SF-C1 api-a evidence missing.');
  console.log('scenario SF-C1 passed');
}

async function waitForSingleLivePeer(baseUrl: string): Promise<readonly PeerDto[]> {
  const deadline = Date.now() + 10000;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (last.length === 1) {
      return last;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-C1 expected one live peer after owner lease expiry, last=${JSON.stringify(last)}`);
}

async function waitForProviderReply(baseUrl: string, rid: string, prefix: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let index = 0;
  let consecutive = 0;
  while (Date.now() < deadline) {
    const value = `${prefix}-${index++}`;
    try {
      const reply = await postJson<ProfileRes>(baseUrl, '/profile/request', { value });
      ensure(reply.value === `profile:${value}`, `SF-C1 reply value mismatch for ${value}.`);
      if (reply.providerRid === rid) {
        consecutive += 1;
        if (consecutive >= 3) {
          return;
        }
      } else {
        consecutive = 0;
      }
    } catch {
      // Wait for the routing table to converge after the provider crash.
      consecutive = 0;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-C1 expected request routing to reach ${rid}.`);
}
