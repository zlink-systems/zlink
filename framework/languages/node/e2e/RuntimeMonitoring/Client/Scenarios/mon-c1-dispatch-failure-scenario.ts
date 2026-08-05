// MON-C1: 느리거나 실패한 observer가 다른 작업을 막지 않는다 시나리오를 검증한다.
import type { ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { type ManagedProcess, startServiceB, waitForPortState } from '../Support/managed-service';
import {
  readRouteStatus,
  routeStatusesFromEvidence,
  waitForRouteStatus,
  delay
} from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

export async function runMonC1(options: ClientOptions): Promise<ManagedProcess> {
  const before = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-C1 initial RouteMesh status did not contain a ready svc-b peer.'
  );

  await postJson(options.serviceUrl, '/observer/slow/start', {});
  await postJson(options.serviceUrl, '/observer/failing/start', {});
  await postJson<string[]>(options.serviceUrl, '/evidence/wait', {
    containsAll: ['observer-slow|sequence=', 'observer-failing|sequence=', 'observer-failed|message=monitoring observer failure for e2e'],
    containsAnyGroups: [],
    timeoutMilliseconds: 10000
  });
  const slowSequence = readSlowObserverSequence(await getJson<readonly string[]>(options.serviceUrl, '/evidence'));

  await postJson<object>(options.serviceBUrl, '/shutdown', {});
  await waitForPortState(options.serviceBUrl, false, 'MON-C1 expected svc-b to stop while the slow observer was blocked.');
  const removed = await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-C1 normal observer did not expose the stopped svc-b peer.'
  );

  const restarted = startServiceB(options, 'svc-b-mon-c1');
  try {
    await waitForPortState(options.serviceBUrl, true, 'MON-C1 expected svc-b to restart.');
    const restored = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.isReady
        && status.peers.some((peer) => peer.nodeRid === 'svc-b' && peer.state === 1)
        && status.channels.some((channel) => channel.channelName === 'monitor.profile' && channel.isReady)
        && BigInt(status.sequence) > BigInt(removed.sequence),
      'MON-C1 normal observer did not expose the restored svc-b peer.'
    );

    const request: ProfileReq = { value: 'observer', marker: `mon-c1-${Date.now()}` };
    const reply = await postJson<ProfileRes>(options.triggerUrl, '/profile/request/service-b', request);
    ensure(reply.value === 'profile:observer' && reply.providerRid === 'svc-b', 'MON-C1 request did not complete while observers were blocked.');
    await postJson<string[]>(options.serviceBUrl, '/evidence/wait', {
      containsAll: [`profile-request|rid=svc-b|marker=${request.marker}|value=observer`],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });

    const status = await readRouteStatus(options.serviceUrl);
    ensure(status.isReady, 'MON-C1 normal RouteMesh status was not available after observer failure.');
    ensure(BigInt(status.sequence) >= BigInt(restored.sequence), 'MON-C1 current status moved backwards after recovery.');
    ensure(BigInt(status.sequence) > BigInt(slowSequence), 'MON-C1 status did not advance while the slow observer was blocked.');
    ensure(BigInt(restored.sequence) > BigInt(before.sequence), 'MON-C1 RouteMesh status did not record the peer transition.');

    await waitForObservedRoute(options.serviceUrl, (observed) =>
      observed.some((candidate) => !candidate.isReady
        && candidate.peers.some((peer) => peer.nodeRid === 'svc-b' && peer.state !== 1))
      && observed.some((candidate) => candidate.peers.some((peer) => peer.nodeRid === 'svc-b'))
      && observed.some((candidate) => BigInt(candidate.sequence) >= BigInt(restored.sequence))
    );

    await postJson(options.serviceUrl, '/observer/slow/release', {});
    console.log('scenario MON-C1 passed');
    return restarted;
  } catch (error) {
    await restarted.stop();
    throw error;
  }
}

function readSlowObserverSequence(lines: readonly string[]): string {
  const line = lines.find((entry) => entry.startsWith('observer-slow|sequence='));
  if (line === undefined) throw new Error('MON-C1 slow observer did not receive its first status callback.');
  const sequence = line.slice('observer-slow|sequence='.length);
  if (!/^\d+$/.test(sequence)) throw new Error('MON-C1 slow observer returned an invalid sequence.');
  return sequence;
}

async function waitForObservedRoute(
  url: string,
  predicate: (statuses: ReturnType<typeof routeStatusesFromEvidence>) => boolean
): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    const statuses = routeStatusesFromEvidence(await getJson<readonly string[]>(url, '/evidence'));
    if (predicate(statuses)) return;
    await delay();
  }
  throw new Error('MON-C1 normal observer did not provide removal, restoration, and current status evidence.');
}
