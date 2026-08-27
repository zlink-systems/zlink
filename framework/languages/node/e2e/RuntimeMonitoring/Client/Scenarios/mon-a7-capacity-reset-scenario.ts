// MON-A7: Core HWM과 Application job queue snapshot을 reset한다 시나리오를 검증한다.
import { getJson, postJson } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';
import { waitForRouteStatusAt } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface CapacityStatus {
  readonly measurementEpoch: string;
  readonly coreHwm: {
    readonly effectiveBudgetBytes: string;
    readonly currentAccountedBytes: string;
    readonly peakAccountedBytes: string;
    readonly completionCurrentAccountedBytes: string;
    readonly completionPeakAccountedBytes: string;
  };
  readonly applicationJobQueue: {
    readonly configuredManualMax?: string;
    readonly effectiveMaxQueuedApplicationJobs: string;
    readonly reservedSupplyPermits: string;
    readonly queuedApplicationJobs: string;
    readonly permitsInUse: string;
    readonly peakPermitsInUse: string;
    readonly capacityWaiters: string;
    readonly capacityWaitCount: string;
    readonly capacityWaitDurationSeconds: number;
  };
}

interface SpotCreateRes {
  readonly state: string;
  readonly spotId?: string;
}

export async function runMonA7(options: ClientOptions): Promise<void> {
  await postJson<object>(options.serviceBUrl, '/admin/placement-exclude', {});
  await postJson<object>(options.throwServiceUrl, '/admin/placement-exclude', {});
  let spotId: string | undefined;
  try {
    await waitForRouteStatusAt(
      options.serviceUrl,
      '/status/route/spot',
      status => status.placement.isAvailable,
      'MON-A7 source did not retain Spot placement capacity.'
    );
    const spot = await postJson<SpotCreateRes>(options.serviceUrl, '/spot/create', {});
    ensure(spot.state === 'created' && spot.spotId !== undefined, 'MON-A7 Spot was not created on the source service.');
    spotId = spot.spotId;
    await waitEvidence(options.serviceUrl, `spot-ready|rid=svc-a|spot=${spotId}`);
    const target = `spot:${spotId}`;
    await postJson(options.serviceUrl, '/admin/publish-gate', { target, blocked: true });
    await postJson(options.serviceUrl, '/admin/capacity-gate/arm', {});

    const first = postJson(options.serviceUrl, '/admin/capacity-publish', { marker: 'mon-a7-first' });
    await waitEvidence(options.serviceUrl, `publish-entered|rid=svc-a|spot=${spotId}|marker=mon-a7-first`);
    const held = await capacity(options);
    ensure(held.applicationJobQueue.effectiveMaxQueuedApplicationJobs === '1', 'MON-A7 queue limit was not one.');
    ensure(held.applicationJobQueue.queuedApplicationJobs === '1', 'MON-A7 first handler did not retain its queued permit.');
    ensure(held.applicationJobQueue.permitsInUse === '1', 'MON-A7 first handler did not consume the only permit.');

    const second = postJson(options.serviceUrl, '/admin/capacity-publish', { marker: 'mon-a7-second' });
    const saturated = await waitCapacity(
      options,
      value => value.applicationJobQueue.capacityWaiters === '1',
      'MON-A7 did not observe one capacity waiter.'
    );
    ensure(saturated.measurementEpoch === held.measurementEpoch, 'MON-A7 pre-reset snapshot crossed a measurement epoch.');
    ensure(saturated.coreHwm.effectiveBudgetBytes !== undefined, 'MON-A7 Core HWM snapshot is incomplete.');

    await postJson(options.serviceUrl, '/admin/publish-gate', { target, blocked: false });
    await Promise.all([first, second]);
    const before = await waitCapacity(
      options,
      value => value.applicationJobQueue.capacityWaitCount !== '0'
        && value.applicationJobQueue.capacityWaitDurationSeconds >= 0,
      'MON-A7 capacity wait metrics were not recorded.'
    );
    const reset = await postJson<CapacityStatus>(options.serviceUrl, '/admin/capacity/reset', {});
    ensure(BigInt(reset.measurementEpoch) === BigInt(before.measurementEpoch) + 1n, 'MON-A7 reset did not advance the shared epoch exactly once.');
    ensure(reset.applicationJobQueue.configuredManualMax === before.applicationJobQueue.configuredManualMax, 'MON-A7 reset changed queue configuration.');
    ensure(reset.applicationJobQueue.effectiveMaxQueuedApplicationJobs === before.applicationJobQueue.effectiveMaxQueuedApplicationJobs, 'MON-A7 reset changed effective queue capacity.');
    ensure(reset.applicationJobQueue.reservedSupplyPermits === '0' && reset.applicationJobQueue.queuedApplicationJobs === '0' && reset.applicationJobQueue.permitsInUse === '0', 'MON-A7 reset changed current queue gauges.');
    ensure(reset.applicationJobQueue.peakPermitsInUse === reset.applicationJobQueue.permitsInUse, 'MON-A7 reset did not set queue peak to current usage.');
    ensure(reset.applicationJobQueue.capacityWaitCount === '0' && reset.applicationJobQueue.capacityWaitDurationSeconds === 0, 'MON-A7 reset did not clear queue counters.');
  } finally {
    if (spotId !== undefined) await postJson(options.serviceUrl, '/spot/close', { spotId }).catch(() => undefined);
    await postJson(options.serviceUrl, '/admin/publish-gate', { target: `spot:${spotId ?? ''}`, blocked: false }).catch(() => undefined);
    await postJson(options.serviceBUrl, '/admin/placement-include', {}).catch(() => undefined);
    await postJson(options.throwServiceUrl, '/admin/placement-include', {}).catch(() => undefined);
  }
}

async function capacity(options: ClientOptions): Promise<CapacityStatus> {
  return await getJson<CapacityStatus>(options.serviceUrl, '/status/capacity');
}

async function waitCapacity(
  options: ClientOptions,
  predicate: (value: CapacityStatus) => boolean,
  message: string
): Promise<CapacityStatus> {
  const deadline = Date.now() + 20_000;
  while (Date.now() < deadline) {
    const value = await capacity(options);
    if (predicate(value)) return value;
    await new Promise(resolve => setTimeout(resolve, 50));
  }
  throw new Error(message);
}

async function waitEvidence(url: string, expected: string): Promise<void> {
  await postJson(url, '/evidence/wait', {
    containsAll: [expected],
    containsAnyGroups: [],
    timeoutMilliseconds: 20_000
  });
}
