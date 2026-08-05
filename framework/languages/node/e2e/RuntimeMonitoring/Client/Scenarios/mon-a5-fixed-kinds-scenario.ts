// MON-A5: Store 장애와 복구 상태를 관찰한다 시나리오를 검증한다.
import { spawn } from 'node:child_process';
import type { EvidenceWaitReq, ProfileReq, ProfileRes } from '../../Shared/messages';
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { routeStatusesFromEvidence, waitForRouteStatus, delay } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

export async function runMonA5(options: ClientOptions): Promise<void> {
  const before = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.channels.some((channel) => channel.isReady),
    'MON-A5 RouteMesh was not ready before the Store outage.'
  );

  await docker('pause', options.redisContainer);
  try {
    const degraded = await waitForRouteStatus(
      options.serviceUrl,
      (status) => !status.isReady && (status.state === 2 || status.placement.unavailableReason === 3),
      'MON-A5 RouteMesh did not expose Store unavailability.'
    );
    ensure(BigInt(degraded.sequence) >= BigInt(before.sequence), 'MON-A5 RouteMesh sequence moved backwards during Store outage.');
    const observed = routeStatusesFromEvidence(await getJson<readonly string[]>(options.serviceUrl, '/evidence'));
    ensure(
      observed.some((status) => !status.isReady && (status.state === 2 || status.placement.unavailableReason === 3)),
      'MON-A5 public observer did not expose the degraded Store status.'
    );
  } finally {
    await docker('unpause', options.redisContainer);
  }

  const restored = await waitForRouteStatus(
    options.serviceUrl,
    (status) => status.isReady && status.channels.some((channel) => channel.isReady),
    'MON-A5 RouteMesh did not recover after the Store was resumed.'
  );
  ensure(BigInt(restored.sequence) > BigInt(before.sequence), 'MON-A5 RouteMesh sequence did not advance after Store recovery.');
  const reply = await postJson<ProfileRes>(options.triggerUrl, '/profile/request/service-b', {
    value: 'after-recovery',
    marker: 'mon-a5-after-recovery'
  } satisfies ProfileReq);
  ensure(reply.providerRid === 'svc-b' && reply.value === 'profile:after-recovery', 'MON-A5 request did not recover after Store restoration.');
  await postJson<string[]>(options.serviceBUrl, '/evidence/wait', {
    containsAll: ['profile-request|rid=svc-b|marker=mon-a5-after-recovery|value=after-recovery'],
    containsAnyGroups: [],
    timeoutMilliseconds: 10000
  } satisfies EvidenceWaitReq);
  console.log('scenario MON-A5 passed');
}

async function docker(verb: 'pause' | 'unpause', container: string): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    const child = spawn('docker', [verb, container], { stdio: 'ignore' });
    child.once('exit', (code) => {
      if (code === 0) resolve();
      else reject(new Error(`docker ${verb} ${container} failed with exit code ${code}`));
    });
    child.once('error', reject);
  });
  await delay(100);
}
