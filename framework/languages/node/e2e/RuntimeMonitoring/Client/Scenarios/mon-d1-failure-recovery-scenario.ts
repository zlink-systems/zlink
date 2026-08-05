// MON-D1: public validation과 장애 반복 중 연속성 시나리오를 검증한다.
import type { EvidenceWaitReq, ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { type ManagedProcess, startServiceB, waitForPortState } from '../Support/managed-service';
import {
  routeStatusesFromEvidence,
  waitForRouteStatus,
  delay
} from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

export async function runMonD1(options: ClientOptions): Promise<ManagedProcess> {
  const result = await restartAndProbe(options, 'mon-d1-request');
  console.log('scenario MON-D1 passed');
  return result.process;
}

export async function runMonD1B(options: ClientOptions): Promise<ManagedProcess> {
  let current: ManagedProcess | undefined;
  try {
    for (let cycle = 1; cycle <= 3; cycle += 1) {
      const result = await restartAndProbe(options, `mon-d1b-cycle-${cycle}`);
      current = result.process;
    }
    console.log('scenario repeated restart passed');
    return current!;
  } catch (error) {
    await current?.stop();
    throw error;
  }
}

async function restartAndProbe(
  options: ClientOptions,
  marker: string
): Promise<{ readonly process: ManagedProcess; readonly routeSequence: string }> {
  const before = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-D1 expected svc-b to be ready before restart.'
  );
  await postJson<object>(options.serviceBUrl, '/shutdown', {});
  await waitForPortState(options.serviceBUrl, false, 'MON-D1 expected service-b to stop.');
  const removed = await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'MON-D1 observer did not expose the removed peer.'
  );

  const restarted = startServiceB(options, `svc-b-${marker}`);
  try {
    await waitForPortState(options.serviceBUrl, true, 'MON-D1 expected service-b to restart.');
    const restored = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b')
        && status.readyPeerCount > removed.readyPeerCount,
      'MON-D1 observer did not expose the restarted peer.'
    );
    ensure(BigInt(restored.sequence) > BigInt(before.sequence), 'MON-D1 RouteMesh sequence did not increase after restart.');

    const reply = await postJson<ProfileRes>(options.triggerUrl, '/profile/request/service-b', {
      value: 'restart',
      marker
    } satisfies ProfileReq);
    ensure(reply.providerRid === 'svc-b' && reply.marker === marker && reply.value === 'profile:restart', 'MON-D1 restarted service did not handle request.');
    const evidence = await postJson<string[]>(options.serviceBUrl, '/evidence/wait', {
      containsAll: [`profile-request|rid=svc-b|marker=${marker}|value=restart`],
      containsAnyGroups: [],
      timeoutMilliseconds: 15000
    } satisfies EvidenceWaitReq);
    ensure(evidence.some((line) => line.includes(`profile-request|rid=svc-b|marker=${marker}|value=restart`)), 'MON-D1 restarted service evidence missing.');

    const observed = routeStatusesFromEvidence(await getJson<readonly string[]>(options.serviceUrl, '/evidence'));
    ensure(
      observed.some((status) => status.peers.every((peer) => peer.nodeRid !== 'svc-b'))
        && observed.some((status) => status.peers.some((peer) => peer.nodeRid === 'svc-b')),
      'MON-D1 observer did not retain a current status after peer restart.'
    );
    return { process: restarted, routeSequence: restored.sequence };
  } catch (error) {
    await restarted.stop();
    throw error;
  }
}

export async function waitForRouteObservation(options: ClientOptions, sequence: string): Promise<void> {
  const deadline = Date.now() + 15_000;
  while (Date.now() < deadline) {
    const statuses = routeStatusesFromEvidence(await getJson<readonly string[]>(options.serviceUrl, '/evidence'));
    if (statuses.some((status) => BigInt(status.sequence) >= BigInt(sequence))) return;
    await delay();
  }
  throw new Error('MON-D1 public RouteMesh observer did not catch up to the latest status.');
}
