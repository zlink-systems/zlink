// MON-A6: Placement 집계와 capacity 결과를 대조한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { postJson } from '../../../http-client';
import { waitForRouteStatusAt } from '../Support/public-status';
import { ensure } from '../Support/scenario-assert';

interface SpotResult {
  readonly state: string;
  readonly spotId?: string;
  readonly errorKind?: string;
}

interface ActorResult {
  readonly state: string;
  readonly actorId?: string;
  readonly errorKind?: string;
}

// The public Node error enum assigns CapacityExceeded the wire value 6.
const capacityExceededErrorKind = '6';

export async function runMonA6(options: ClientOptions): Promise<void> {
  await postJson<object>(options.serviceBUrl, '/admin/placement-exclude', {});
  await postJson<object>(options.throwServiceUrl, '/admin/placement-exclude', {});
  try {
    await waitForRouteStatusAt(
      options.serviceBUrl,
      '/status/route/spot',
      (status) => !status.placement.isAvailable,
      'MON-A6 svc-b remained a placement target after exclusion.'
    );
    await waitForRouteStatusAt(
      options.throwServiceUrl,
      '/status/route/spot',
      (status) => !status.placement.isAvailable,
      'MON-A6 svc-throw remained a placement target after exclusion.'
    );

    const firstSpot = await createSpot(options);
    ensure(firstSpot.state === 'created' && firstSpot.spotId !== undefined, 'MON-A6 first User Spot was not created.');

    const actorId = `monitor-actor-${Date.now()}`;
    const actor = await postJson<ActorResult>(options.serviceUrl, '/actor/create', { actorId });
    ensure(actor.state === 'created' && actor.actorId === actorId, 'MON-A6 Actor was not created.');

    await waitForRouteStatusAt(
      options.serviceUrl,
      '/status/route/spot',
      (status) => status.placement.activeSpotCount >= 1 && status.placement.activeActorCount >= 1,
      'MON-A6 public placement status did not include the completed Spot and Actor.'
    );

    const secondSpot = await createSpot(options);
    ensure(secondSpot.state === 'created' && secondSpot.spotId !== undefined, 'MON-A6 capacity did not admit the second Spot.');
    await waitForRouteStatusAt(
      options.serviceUrl,
      '/status/route/spot',
      (status) => status.placement.activeSpotCount >= 2,
      'MON-A6 public placement status did not include the second completed Spot.'
    );

    const rejectedSpot = await createSpot(options);
    ensure(rejectedSpot.state === 'rejected', 'MON-A6 capacity limit accepted an additional Spot.');
    ensure(
      rejectedSpot.errorKind === capacityExceededErrorKind,
      'MON-A6 capacity rejection did not expose CapacityExceeded.'
    );

    try {
      await postJson(options.serviceUrl, '/actor/destroy', { actorId });
      await postJson(options.serviceUrl, '/spot/close', { spotId: firstSpot.spotId });
      await waitForRouteStatusAt(
        options.serviceUrl,
        '/status/route/spot',
        (status) => status.placement.activeActorCount === 0 && status.placement.activeSpotCount <= 1,
        'MON-A6 placement status did not return to available capacity after removal.'
      );

      const replacementSpot = await createSpot(options);
      ensure(
        replacementSpot.state === 'created' && replacementSpot.spotId !== undefined,
        'MON-A6 placement did not admit a Spot after capacity was released.'
      );
      await postJson(options.serviceUrl, '/spot/close', { spotId: replacementSpot.spotId });
    } finally {
      await postJson(options.serviceUrl, '/spot/close', { spotId: secondSpot.spotId }).catch(() => undefined);
      await postJson(options.serviceUrl, '/actor/destroy', { actorId }).catch(() => undefined);
    }

    console.log('scenario MON-A6 passed');
  } finally {
    await postJson<object>(options.serviceBUrl, '/admin/placement-include', {}).catch(() => undefined);
    await postJson<object>(options.throwServiceUrl, '/admin/placement-include', {}).catch(() => undefined);
  }
}

async function createSpot(options: ClientOptions): Promise<SpotResult> {
  return await postJson<SpotResult>(options.serviceUrl, '/spot/create', {});
}
