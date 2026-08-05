// MON-B2: Local target handler 대기가 다른 target 전달을 막지 않는다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { readRouteStatus } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface SpotResult {
  readonly state: string;
  readonly spotId?: string;
}

export async function runMonB2(options: ClientOptions): Promise<void> {
  const first = await postJson<SpotResult>(options.serviceUrl, '/spot/create', {});
  const second = await postJson<SpotResult>(options.serviceUrl, '/spot/create', {});
  ensure(first.state === 'created' && first.spotId !== undefined, 'MON-B2 first local target was not created.');
  ensure(second.state === 'created' && second.spotId !== undefined, 'MON-B2 second local target was not created.');

  const before = await readRouteStatus(options.serviceUrl);
  const marker = `mon-b2-${Date.now()}`;
  try {
    await postJson(options.serviceUrl, '/admin/publish-gate', { target: `spot:${first.spotId}`, blocked: true });
    await postJson(options.serviceUrl, '/admin/publish-gate', { target: 'entry:svc-a', blocked: true });
    await postJson(options.serviceUrl, '/admin/publish', { marker });

    const evidence = await postJson<string[]>(options.serviceUrl, '/evidence/wait', {
      containsAll: [
        `publish-entered|rid=svc-a|spot=${first.spotId}|marker=${marker}`,
        `publish-received|rid=svc-a|spot=${second.spotId}|marker=${marker}`
      ],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });
    ensure(
      evidence.some((line) => line.includes(`publish-received|rid=svc-a|spot=${second.spotId}|marker=${marker}`)),
      'MON-B2 an unblocked local target did not process the marker.'
    );
    ensure(
      !evidence.some((line) => line.includes(`publish-received|rid=svc-a|spot=${first.spotId}|marker=${marker}`)),
      'MON-B2 the blocked local target processed the marker before its gate opened.'
    );
    ensure(
      !(await getJson<string[]>(options.serviceUrl, '/evidence'))
        .some((line) => line.includes(`publish-received|rid=svc-a|spot=${first.spotId}|marker=${marker}`)),
      'MON-B2 the blocked local target reported delivery while its gate was closed.'
    );

    const after = await readRouteStatus(options.serviceUrl);
    ensure(topologyFingerprint(before) === topologyFingerprint(after), 'MON-B2 publish changed topology status.');

    await postJson(options.serviceUrl, '/admin/publish-gate', { target: `spot:${first.spotId}`, blocked: false });
    await postJson<string[]>(options.serviceUrl, '/evidence/wait', {
      containsAll: [`publish-received|rid=svc-a|spot=${first.spotId}|marker=${marker}`],
      containsAnyGroups: [],
      timeoutMilliseconds: 10000
    });
  } finally {
    await postJson(options.serviceUrl, '/admin/publish-gate', { target: 'entry:svc-a', blocked: false }).catch(() => undefined);
    await postJson(options.serviceUrl, '/admin/publish-gate', { target: `spot:${first.spotId}`, blocked: false }).catch(() => undefined);
    await postJson(options.serviceUrl, '/spot/close', { spotId: first.spotId }).catch(() => undefined);
    await postJson(options.serviceUrl, '/spot/close', { spotId: second.spotId }).catch(() => undefined);
  }

  console.log('scenario MON-B2 passed');
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
