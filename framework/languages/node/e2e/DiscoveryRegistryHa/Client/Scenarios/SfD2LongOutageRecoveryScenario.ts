// SF-D2: 긴 장애 뒤 재등록한 provider만 유지한다 시나리오를 검증한다.
import type { ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, getStatus, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface PeerDto {
  readonly endpoint: string;
  readonly nodeRid?: string;
}

export async function runSfD2(options: ClientOptions): Promise<void> {
  console.log('scenario-control SF-D2 stop-redis-and-kill-api-b');
  await waitForProviderStopped(options.providerBUrl);
  const traffic = driveRequests(options.consumerUrl, 10000);
  console.log('scenario-control SF-D2 restart-redis');
  const replies = await traffic;
  ensure(replies.some((reply) => reply.providerRid === 'api-a'), 'SF-D2 no request was served by surviving api-a.');

  await waitForPeer(options.consumerUrl, 'api-a');
  await waitForMissingPeer(options.consumerUrl, 'api-b');

  for (let i = 0; i < 8; i++) {
    const reply = await postJson<ProfileRes>(options.consumerUrl, '/profile/request', { value: `sf-d2-after-${i}` });
    ensure(reply.value === `profile:sf-d2-after-${i}`, `SF-D2 post-recovery request ${i} value mismatch.`);
    ensure(reply.providerRid === 'api-a', `SF-D2 post-recovery request ${i} was served by '${reply.providerRid}'.`);
  }

  console.log('scenario SF-D2 passed');
}

async function delay(milliseconds: number): Promise<void> {
  await new Promise((resolve) => setTimeout(resolve, milliseconds));
}

async function waitForProviderStopped(baseUrl: string): Promise<void> {
  const deadline = Date.now() + 5000;
  while (Date.now() < deadline) {
    const status = await getStatus(`${baseUrl}/health`).catch(() => undefined);
    if (status === undefined || status < 200 || status >= 300) {
      return;
    }
    await delay(50);
  }
  throw new Error('SF-D2 provider api-b did not stop.');
}

async function driveRequests(baseUrl: string, windowMs: number): Promise<ProfileRes[]> {
  const replies: ProfileRes[] = [];
  const deadline = Date.now() + windowMs;
  let index = 0;
  while (Date.now() < deadline) {
    const reply = await postJson<ProfileRes>(baseUrl, '/profile/request', { value: `sf-d2-${index++}` });
    ensure(reply.value.startsWith('profile:sf-d2-'), 'SF-D2 request returned an unexpected value.');
    replies.push(reply);
    await new Promise((resolve) => setTimeout(resolve, 150));
  }

  ensure(replies.length > 0, 'SF-D2 request window produced no successful traffic.');
  return replies;
}

async function waitForPeer(baseUrl: string, rid: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (last.some((peer) => peerHasRid(peer, rid))) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D2 expected live peer ${rid}, last=${JSON.stringify(last)}`);
}

async function waitForMissingPeer(baseUrl: string, rid: string): Promise<void> {
  const deadline = Date.now() + 10000;
  let last: readonly PeerDto[] = [];
  while (Date.now() < deadline) {
    last = await getJson<PeerDto[]>(baseUrl, '/location/peers');
    if (!last.some((peer) => peerHasRid(peer, rid))) {
      return;
    }
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-D2 expected peer ${rid} to be absent, last=${JSON.stringify(last)}`);
}

function peerHasRid(peer: PeerDto, rid: string): boolean {
  return peer.nodeRid === rid;
}
