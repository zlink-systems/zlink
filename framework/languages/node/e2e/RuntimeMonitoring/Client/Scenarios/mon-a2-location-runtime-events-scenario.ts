// MON-A2: Peer가 추가되고 제거된 결과를 관찰한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { type ManagedProcess, startServiceB, waitForPortState } from '../Support/managed-service';
import {
  routeStatusesFromEvidence,
  waitForRouteStatus,
  delay
} from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface PeerObservation {
  readonly rid: string;
  readonly endpoint: string;
}

export async function runMonA2(options: ClientOptions): Promise<ManagedProcess> {
  const baseline = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.peers.some((peer) => peer.nodeRid === 'svc-b') && status.isReady,
    'MON-A2 initial RouteMesh status did not contain a ready svc-b peer.'
  );
  const baselinePeers = await getJson<readonly PeerObservation[]>(options.serviceUrl, '/locations/peers');
  ensure(baselinePeers.some((peer) => peer.rid === 'svc-b'), 'MON-A2 initial topology query did not contain svc-b.');

  await postJson<object>(options.serviceBUrl, '/shutdown', {});
  await waitForPortState(options.serviceBUrl, false, 'MON-A2 expected svc-b to stop.');
  const removed = await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-A2 RouteMesh observer did not remove the stopped peer.'
  );
  const removedPeers = await waitForTopologyRows(options.serviceUrl, (rows) => !rows.some((peer) => peer.rid === 'svc-b'));
  ensure(removedPeers.every((peer) => peer.rid !== 'svc-b'), 'MON-A2 topology query retained the stopped peer.');

  const restarted = startServiceB(options, 'svc-b-mon-a2');
  try {
    await waitForPortState(options.serviceBUrl, true, 'MON-A2 expected svc-b to restart.');
    const restored = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.peers.some((peer) => peer.nodeRid === 'svc-b')
        && status.isReady
        && status.readyPeerCount > removed.readyPeerCount,
      'MON-A2 RouteMesh status did not restore the restarted peer.'
    );
    const restoredPeers = await waitForTopologyRows(options.serviceUrl, (rows) => rows.some((peer) => peer.rid === 'svc-b'));
    ensure(restoredPeers.some((peer) => peer.rid === 'svc-b'), 'MON-A2 topology query did not restore svc-b.');
    ensure(BigInt(restored.sequence) > BigInt(baseline.sequence), 'MON-A2 RouteMesh sequence did not increase.');

    const observed = routeStatusesFromEvidence(await getJson<readonly string[]>(options.serviceUrl, '/evidence'));
    ensure(
      observed.some((status) => status.peers.every((peer) => peer.nodeRid !== 'svc-b'))
        && observed.some((status) => status.peers.some((peer) => peer.nodeRid === 'svc-b')),
      'MON-A2 public RouteMesh observer did not expose both removal and restoration states.'
    );
    console.log('scenario MON-A2 passed');
    return restarted;
  } catch (error) {
    await restarted.stop();
    throw error;
  }
}

async function waitForTopologyRows(
  url: string,
  predicate: (rows: readonly PeerObservation[]) => boolean
): Promise<readonly PeerObservation[]> {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    const rows = await getJson<readonly PeerObservation[]>(url, '/locations/peers');
    if (predicate(rows)) return rows;
    await delay();
  }
  throw new Error('MON-A2 Location topology query did not converge to the observed RouteMesh status.');
}
