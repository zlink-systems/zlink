// MON-B1: Remote target 일부가 받지 못해도 topology status를 delivery 결과로 바꾸지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { readRouteStatus, waitForHostStatus } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface SpotResult {
  readonly state: string;
  readonly spotId?: string;
  readonly nodeRid?: string;
}

export async function runMonB1(options: ClientOptions): Promise<void> {
  const created: SpotResult[] = [];
  try {
    const remoteByRid = new Map<string, SpotResult>();
    for (let attempt = 0; attempt < 6 && remoteByRid.size < 2; attempt += 1) {
      const spot = await postJson<SpotResult>(options.serviceUrl, '/spot/create', {});
      if (spot.state === 'created' && spot.spotId !== undefined && spot.nodeRid !== undefined) {
        created.push(spot);
        if (spot.nodeRid !== 'svc-a') remoteByRid.set(spot.nodeRid, spot);
      }
    }

    const remoteSpots = [...remoteByRid.values()];
    ensure(remoteSpots.length >= 2, 'MON-B1 did not place two matching User Spots on remote processes.');
    const blocked = remoteSpots[0]!;
    const available = remoteSpots[1]!;
    const blockedUrl = serviceUrlForRid(options, blocked.nodeRid!);
    const availableUrl = serviceUrlForRid(options, available.nodeRid!);
    const blockerMarker = `mon-b1-blocker-${Date.now()}`;
    const marker = `mon-b1-marker-${Date.now()}`;

    await postJson(blockedUrl, '/admin/publish-gate', { target: `spot:${blocked.spotId}`, blocked: true });
    await postJson(options.serviceUrl, '/admin/publish', { marker: blockerMarker, blockerBytes: 16 * 1024 });
    await postJson<string[]>(blockedUrl, '/evidence/wait', {
      containsAll: [`publish-entered|rid=${blocked.nodeRid}|spot=${blocked.spotId}|marker=${blockerMarker}`],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });
    const blockedHost = await waitForHostStatus(
      blockedUrl,
      (status) => status.inboundDispatch.applicationReceivePaused,
      'MON-B1 blocked target did not expose Application receive paused=true.',
      10000
    );
    ensure(
      BigInt(blockedHost.inboundDispatch.applicationHwmBytes) < 16_384n,
      'MON-B1 blocker payload was not larger than the configured public HWM.'
    );

    const before = await readRouteStatus(options.serviceUrl);
    await postJson(options.serviceUrl, '/admin/publish', { marker });

    const availableEvidence = await postJson<string[]>(availableUrl, '/evidence/wait', {
      containsAll: [`publish-received|rid=${available.nodeRid}|spot=${available.spotId}|marker=${marker}`],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });
    ensure(
      availableEvidence.some((line) => line.includes(`publish-received|rid=${available.nodeRid}|spot=${available.spotId}|marker=${marker}`)),
      'MON-B1 an available remote target did not process the Logical Multicast marker.'
    );

    const blockedEvidence = await getJson<string[]>(blockedUrl, '/evidence');
    ensure(
      !blockedEvidence.some((line) => line.includes(`publish-received|rid=${blocked.nodeRid}|spot=${blocked.spotId}|marker=${marker}`)),
      'MON-B1 a blocked remote target reported delivery before its application gate opened.'
    );

    const after = await readRouteStatus(options.serviceUrl);
    ensure(topologyFingerprint(before) === topologyFingerprint(after), 'MON-B1 publish changed topology status.');

    await postJson(blockedUrl, '/admin/publish-gate', { target: `spot:${blocked.spotId}`, blocked: false });
    await postJson<string[]>(blockedUrl, '/evidence/wait', {
      containsAll: [`publish-received|rid=${blocked.nodeRid}|spot=${blocked.spotId}|marker=${marker}`],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });

    console.log('scenario MON-B1 passed');
  } finally {
    for (const spot of created) {
      if (spot.spotId !== undefined) {
        await postJson(options.serviceUrl, '/spot/close', { spotId: spot.spotId }).catch(() => undefined);
      }
    }
  }
}

function serviceUrlForRid(options: ClientOptions, rid: string): string {
  if (rid === 'svc-a') return options.serviceUrl;
  if (rid === 'svc-b') return options.serviceBUrl;
  if (rid === 'svc-throw') return options.throwServiceUrl;
  throw new Error(`MON-B1 has no public evidence endpoint for owner '${rid}'.`);
}

function topologyFingerprint(status: {
  readonly state: number;
  readonly isReady: boolean;
  readonly readyPeerCount: number;
  readonly channels: readonly unknown[];
  readonly peers: readonly unknown[];
  readonly placement: unknown;
}): string {
  return JSON.stringify({
    state: status.state,
    isReady: status.isReady,
    readyPeerCount: status.readyPeerCount,
    channels: status.channels,
    peers: status.peers,
    placement: status.placement
  });
}
