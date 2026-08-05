// MON-A1: Host와 RouteMesh status를 각각 읽는다 시나리오를 검증한다.
import type { EvidenceWaitReq, ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { type ManagedProcess, startServiceB, waitForPortState } from '../Support/managed-service';
import {
  hostStatusesFromEvidence,
  readHostStatus,
  readRouteStatus,
  routeStatusesFromEvidence,
  waitForRouteStatus,
  delay
} from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

export async function runMonA1(options: ClientOptions): Promise<ManagedProcess> {
  const hostBefore = await readHostStatus(options.serviceUrl);
  const routeBefore = await readRouteStatus(options.serviceUrl);
  ensure(hostBefore.isReady && hostBefore.acceptingWork, 'MON-A1 Host was not serving before the peer transition.');

  await postJson<object>(options.serviceBUrl, '/shutdown', {});
  await waitForPortState(options.serviceBUrl, false, 'MON-A1 expected the initial svc-b process to stop.');
  const routeWithoutPeer = await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-A1 RouteMesh status retained the stopped peer.'
  );

  const restarted = startServiceB(options, 'svc-b-mon-a1');
  try {
    await waitForPortState(options.serviceBUrl, true, 'MON-A1 expected svc-b to start.');
    const routeAfter = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.peers.some((peer) => peer.nodeRid === 'svc-b' && peer.state === 1)
        && status.channels.some((channel) => channel.channelName === 'monitor.profile' && channel.isReady),
      'MON-A1 RouteMesh status did not restore the peer and Channel readiness.'
    );
    ensure(
      BigInt(routeAfter.sequence) > BigInt(routeBefore.sequence),
      'MON-A1 RouteMesh sequence did not increase in its own source.'
    );
    ensure(
      JSON.stringify(routeBefore) !== JSON.stringify(routeAfter),
      'MON-A1 previously returned RouteMesh status was mutated by a later transition.'
    );

    const hostAfter = await readHostStatus(options.serviceUrl);
    ensure(hostAfter.isReady && hostAfter.acceptingWork, 'MON-A1 Host stopped accepting work during peer recovery.');
    ensure(
      BigInt(hostAfter.sequence) >= BigInt(hostBefore.sequence),
      'MON-A1 Host sequence was compared outside its source or moved backwards.'
    );

    const request: ProfileReq = { value: 'monitor', marker: 'mon-a1-request' };
    const reply = await postJson<ProfileRes>(options.triggerUrl, '/profile/request/service-b', request);
    ensure(reply.value === 'profile:monitor' && reply.providerRid === 'svc-b', 'MON-A1 request did not reach the expected provider.');
    await postJson<string[]>(options.serviceBUrl, '/evidence/wait', {
      containsAll: ['profile-request|rid=svc-b|marker=mon-a1-request|value=monitor'],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    } satisfies EvidenceWaitReq);

    await waitForObservedRoute(options.serviceUrl, (status) => BigInt(status.sequence) >= BigInt(routeAfter.sequence));
    await waitForObservedHost(options.serviceUrl, (status) => BigInt(status.sequence) >= BigInt(hostAfter.sequence));
    console.log('scenario MON-A1 passed');
    return restarted;
  } catch (error) {
    await restarted.stop();
    throw error;
  }
}

async function waitForObservedRoute(
  url: string,
  predicate: (status: ReturnType<typeof routeStatusesFromEvidence>[number]) => boolean
): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    const statuses = routeStatusesFromEvidence(await getJson<readonly string[]>(url, '/evidence'));
    if (statuses.some(predicate)) return;
    await delay();
  }
  throw new Error('MON-A1 RouteMesh observer did not provide the current public status.');
}

async function waitForObservedHost(
  url: string,
  predicate: (status: ReturnType<typeof hostStatusesFromEvidence>[number]) => boolean
): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    const statuses = hostStatusesFromEvidence(await getJson<readonly string[]>(url, '/evidence'));
    if (statuses.some(predicate)) return;
    await delay();
  }
  throw new Error('MON-A1 Host observer did not provide the current public status.');
}
