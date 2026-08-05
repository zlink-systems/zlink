// MON-A4: 정상 replacement과 crash 뒤 readiness를 복원하는 시나리오를 검증한다.
import type { ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import {
  type ManagedProcess,
  startReplacementService,
  startServiceB,
  waitForPortState
} from '../Support/managed-service';
import { waitForRouteStatus, delay } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface RelocationResult {
  readonly outcome: number;
  readonly reason: number;
}

interface PeerObservation {
  readonly rid: string;
  readonly endpoint: string;
}

export async function runMonA4A(options: ClientOptions): Promise<ManagedProcess> {
  const before = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'normal replacement initial provider was not ready.'
  );
  const drain = await postJsonWithin<RelocationResult>(options.serviceBUrl, '/admin/drain', {}, 35_000);
  ensure(drain.outcome === 0 && drain.reason === 0, 'normal replacement provider relocation did not complete.');
  await postJson<object>(options.serviceBUrl, '/shutdown', {});
  await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'normal replacement old provider remained in RouteMesh status after drain.'
  );
  await waitForPortState(options.serviceBUrl, false, 'normal replacement expected the old provider endpoint to stop.');

  const replacement = startReplacementService(options, 'svc-b-mon-a4-replacement');
  try {
    await waitForPortState(options.replacementServiceUrl, true, 'normal replacement expected the replacement provider to start.');
    const after = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b')
        && status.readyPeerCount > 0,
      'normal replacement RouteMesh did not become ready after replacement.'
    );
    const topology = await waitForPeers(options.serviceUrl, (rows) =>
      rows.some((row) => row.rid === 'svc-b' && row.endpoint === options.replacementServiceChannelEndpoint)
      && rows.every((row) => row.endpoint !== options.serviceBChannelEndpoint));
    ensure(topology.some((row) => row.endpoint === options.replacementServiceChannelEndpoint), 'normal replacement endpoint was not published.');
    ensure(BigInt(after.sequence) > BigInt(before.sequence), 'normal replacement RouteMesh sequence did not advance after replacement.');

    const reply = await requestProfile(options, '/profile/request/replacement', 'mon-a4a-replacement');
    ensure(reply.providerRid === 'svc-b' && reply.value === 'profile:replacement', 'normal replacement request did not reach the replacement provider.');
    console.log('scenario normal replacement passed');
    return replacement;
  } catch (error) {
    await replacement.stop();
    throw error;
  }
}

export async function runMonA4B(options: ClientOptions): Promise<ManagedProcess> {
  const useReplacement = await isHealthy(options.replacementServiceUrl);
  const currentUrl = useReplacement ? options.replacementServiceUrl : options.serviceBUrl;
  const requestPath = useReplacement ? '/profile/request/replacement' : '/profile/request/service-b';
  const currentConfig = useReplacement ? 'replacement' : 'service-b';
  const before = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'crash recovery provider was not ready before crash.'
  );

  await postJson<object>(currentUrl, '/crash', {});
  await waitForPortState(currentUrl, false, 'crash recovery expected the crashed provider endpoint to stop.');
  await waitForRouteStatus(
    options.serviceUrl,
    (status) => !status.peers.some((peer) => peer.nodeRid === 'svc-b'),
    'crash recovery stale crashed peer remained in RouteMesh status.'
  );

  const restarted = useReplacement
    ? startReplacementService(options, 'svc-b-mon-a4b-restart')
    : startServiceB(options, 'svc-b-mon-a4-restart');
  try {
    await waitForPortState(currentUrl, true, `crash recovery expected the ${currentConfig} provider to restart.`);
    const after = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.isReady && status.peers.some((peer) => peer.nodeRid === 'svc-b')
        && status.readyPeerCount > 0,
      'crash recovery RouteMesh did not restore a ready replacement after crash.'
    );
    ensure(BigInt(after.sequence) > BigInt(before.sequence), 'crash recovery RouteMesh sequence did not advance after crash recovery.');
    const reply = await requestProfile(options, requestPath, 'mon-a4b-recovery');
    ensure(reply.providerRid === 'svc-b' && reply.value === 'profile:recovery', 'crash recovery request did not reach the replacement provider.');

    await postJson<object>(currentUrl, '/admin/exclude', {});
    await waitForWeight(currentUrl, 0);
    const excluded = await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.channels.some((channel) =>
        channel.channelName === 'monitor.profile' && !channel.isReady && channel.readyTargetCount === 0),
      'crash recovery excluded provider remained a ready target.'
    );
    let rejected = false;
    try {
      await requestProfile(options, requestPath, 'mon-a4b-excluded');
    } catch {
      rejected = true;
    }
    ensure(rejected, 'crash recovery request succeeded while the only target was excluded.');
    await postJson<object>(currentUrl, '/admin/include', {});
    await waitForWeight(currentUrl, 100);
    await waitForRouteStatus(
      options.serviceUrl,
      (status) => status.channels.some((channel) =>
        channel.channelName === 'monitor.profile' && channel.isReady && channel.readyTargetCount === 1),
      'crash recovery excluded provider did not become ready after inclusion.'
    );
    ensure(BigInt(excluded.sequence) > BigInt(after.sequence), 'crash recovery exclusion did not create a newer public status.');
    console.log('scenario crash recovery passed');
    return restarted;
  } catch (error) {
    await restarted.stop();
    throw error;
  }
}

export async function runMonA4(options: ClientOptions): Promise<ManagedProcess> {
  await runMonA4A(options);
  return await runMonA4B(options);
}

async function requestProfile(options: ClientOptions, path: string, marker: string): Promise<ProfileRes> {
  const value = marker.replace('mon-a4a-', '').replace('mon-a4b-', '');
  return await postJson<ProfileRes>(options.triggerUrl, path, { value, marker } satisfies ProfileReq);
}

async function waitForPeers(
  serviceUrl: string,
  predicate: (rows: readonly PeerObservation[]) => boolean
): Promise<readonly PeerObservation[]> {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    const rows = await getJson<readonly PeerObservation[]>(serviceUrl, '/locations/peers');
    if (predicate(rows)) return rows;
    await delay();
  }
  throw new Error('public topology query did not converge.');
}

async function waitForWeight(serviceUrl: string, expected: number): Promise<void> {
  const deadline = Date.now() + 10_000;
  while (Date.now() < deadline) {
    if ((await getJson<{ readonly weight: number }>(serviceUrl, '/admin/weight')).weight === expected) return;
    await delay();
  }
  throw new Error(`service weight did not become ${expected}.`);
}

async function isHealthy(url: string): Promise<boolean> {
  try {
    await getJson(url, '/health');
    return true;
  } catch {
    return false;
  }
}
