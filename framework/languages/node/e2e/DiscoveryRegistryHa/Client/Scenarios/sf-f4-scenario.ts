// SF-F4: ObjectGeneration과 owner replacement를 public ref로 구분한다 시나리오를 검증한다.
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import type { ClientOptions } from '../Support/client-options';
import { ensure } from '../Support/scenario-assert';

interface LocationReply {
  readonly found: boolean;
  readonly ownerNodeRid?: string;
  readonly objectGeneration?: string;
}

export async function runSFF4(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-F4 provider B URL is required.');
  const actorId = 'sf-f4-actor';
  const userSpotId = 'sf-f4-user-spot';
  const instanceSpotId = 'sf-f4-instance-spot';
  const actor = await postJson<{ status: string; objectGeneration: string }>(
    options.providerAUrl,
    '/capacity/actors',
    { actorId, state: 'sf-f4-actor-state' }
  );
  ensure(actor.status === 'created', 'SF-F4 source Actor creation failed.');
  await postJson(options.providerAUrl, '/capacity/spots', {
    spotId: userSpotId,
    state: 'sf-f4-user-state'
  });
  await postJson(options.providerAUrl, '/capacity/actors/join', { actorId, spotId: userSpotId });
  await requestInstance(options, instanceSpotId, 'create');

  const initialActor = await location(options, 'actor', actorId);
  const initialInstance = await location(options, 'spot', instanceSpotId);
  ensure(initialActor.ownerNodeRid === 'api-a' && initialInstance.ownerNodeRid === 'api-a',
    'SF-F4 initial objects are not owned by api-a.');
  ensure(initialActor.objectGeneration === actor.objectGeneration,
    'SF-F4 Actor create and location generations differ.');

  await postJson(options.providerAUrl, '/placement/weight', { weight: 0 });
  console.log('scenario-control SF-F4 start-provider-b');
  await waitFor(async () => {
    try { await getJson(options.providerBUrl!, '/health'); return true; } catch { return false; }
  });
  const relocation = await postJsonWithin<{ outcome: number }>(
    options.providerAUrl,
    '/drain',
    { deadlineMs: 60_000, stopOnSuccess: false },
    70_000
  );
  ensure(relocation.outcome === 0, 'SF-F4 relocation did not complete.');
  await waitFor(async () => {
    const [currentActor, currentInstance] = await Promise.all([
      location(options, 'actor', actorId),
      location(options, 'spot', instanceSpotId)
    ]);
    return currentActor.ownerNodeRid === 'api-b'
      && currentInstance.ownerNodeRid === 'api-b';
  });
  const relocatedActor = await location(options, 'actor', actorId);
  const relocatedInstance = await location(options, 'spot', instanceSpotId);
  ensure(relocatedActor.objectGeneration === initialActor.objectGeneration,
    'SF-F4 relocation changed Actor ObjectGeneration.');
  ensure(relocatedInstance.objectGeneration === initialInstance.objectGeneration,
    'SF-F4 relocation changed Instance Spot ObjectGeneration.');

  const destroyed = await postJson<{ destroyed: boolean }>(
    options.providerBUrl,
    '/capacity/actors/destroy',
    { actorId }
  );
  ensure(destroyed.destroyed, 'SF-F4 current Actor destroy failed.');
  await waitFor(async () => {
    try {
      await requestInstance(options, instanceSpotId, '__close__');
      return true;
    } catch {
      return false;
    }
  });
  await waitFor(
    async () => !(await location(options, 'actor', actorId)).found,
    30_000,
    'the destroyed Actor authority to disappear'
  );
  await waitFor(
    async () => !(await location(options, 'spot', instanceSpotId)).found,
    30_000,
    'the closed Instance Spot authority to disappear'
  );

  const recreatedActor = await postJson<{ status: string; objectGeneration: string }>(
    options.providerBUrl,
    '/capacity/actors',
    { actorId, state: 'sf-f4-recreated-actor-state' }
  );
  ensure(recreatedActor.status === 'created', 'SF-F4 Actor recreate failed.');
  await waitFor(async () => {
    try {
      await requestInstance(options, instanceSpotId, 'recreate');
      return true;
    } catch {
      return false;
    }
  });
  await waitFor(async () => {
    const current = await location(options, 'spot', instanceSpotId);
    return current.found && current.objectGeneration !== initialInstance.objectGeneration;
  });
  const recreatedInstance = await location(options, 'spot', instanceSpotId);
  ensure(recreatedActor.objectGeneration !== initialActor.objectGeneration,
    'SF-F4 Actor recreate reused ObjectGeneration.');
  ensure(recreatedInstance.objectGeneration !== initialInstance.objectGeneration,
    'SF-F4 Instance Spot recreate reused ObjectGeneration.');

  const staleDestroy = await postJson<{ destroyed: boolean }>(
    options.providerAUrl,
    '/capacity/actors/destroy-exact',
    { actorId, objectGeneration: initialActor.objectGeneration }
  );
  ensure(!staleDestroy.destroyed, 'SF-F4 stale ActorRef destroyed the new incarnation.');
  ensure((await location(options, 'actor', actorId)).objectGeneration === recreatedActor.objectGeneration,
    'SF-F4 stale ActorRef changed the current incarnation.');
  console.log('scenario SF-F4 passed');
}

async function requestInstance(
  options: ClientOptions,
  spotId: string,
  operationId: string
): Promise<void> {
  await postJson(options.consumerUrl, '/object/request', {
    spotId,
    operationId,
    payload: `payload-${operationId}`
  });
}

async function location(
  options: ClientOptions,
  kind: 'actor' | 'spot',
  id: string
): Promise<LocationReply> {
  return await getJson<LocationReply>(
    options.consumerUrl,
    `/location/object?kind=${kind}&id=${encodeURIComponent(id)}`
  );
}

async function waitFor(
  predicate: () => Promise<boolean>,
  timeoutMs = 30_000,
  description = 'the public state transition'
): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await predicate()) return;
    await new Promise((resolve) => setTimeout(resolve, 100));
  }
  throw new Error(`SF-F4 timed out waiting for ${description}.`);
}
