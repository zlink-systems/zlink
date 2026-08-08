// SF-G3: User Spot aggregate capacity를 all-or-none으로 적용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson, postJsonWithin } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface CapacityResult {
  readonly status: string;
  readonly actorId?: string;
  readonly objectGeneration?: string;
}

interface ProbeResult {
  readonly actorId: string;
  readonly actorState: string;
  readonly spotId: string;
  readonly spotState: string;
  readonly nodeRid: string;
}

interface MeshRow {
  readonly rid: string;
  readonly placementWeight: number;
  readonly populationCapacity: {
    readonly actors: { readonly limit: number; readonly active: number };
    readonly spots: { readonly limit: number; readonly active: number };
    readonly spotTypes: readonly { readonly stableType: string; readonly limit: number; readonly active: number }[];
  };
}

interface LocationResult {
  readonly found: boolean;
  readonly ownerNodeRid?: string;
  readonly objectGeneration?: string;
}

export async function runSFG3(options: ClientOptions): Promise<void> {
  ensure(options.providerBUrl !== undefined, 'SF-G3 provider B URL is required.');
  const suffix = `${Date.now()}-${process.pid}`;
  const spotId = `sf-g3-spot-${suffix}`;
  const actorIds = [`sf-g3-actor-a-${suffix}`, `sf-g3-actor-b-${suffix}`];
  const spotState = `spot-state-${suffix}`;
  const actorStates = actorIds.map((_, index) => `actor-state-${index}-${suffix}`);

  const spot = await postJson<{ objectGeneration: string }>(options.providerAUrl, '/capacity/spots', {
    spotId,
    state: spotState
  });
  const actors = await Promise.all(actorIds.map((actorId, index) =>
    postJson<CapacityResult>(options.providerAUrl, '/capacity/actors', {
      actorId,
      state: actorStates[index]
    })));
  ensure(actors.every((result) => result.status === 'created'), 'SF-G3 source Actor creation failed.');
  await Promise.all(actorIds.map((actorId) =>
    postJson(options.providerAUrl, '/capacity/actors/join', { actorId, spotId })));
  await assertAggregate(options.providerAUrl, actorIds, actorStates, spotId, spotState, 'api-a');

  await postJson(options.providerAUrl, '/placement/weight', { weight: 0 });
  console.log('scenario-control SF-G3 start-provider-b');

  const target = await waitForProviderB(options);
  const spotType = target.populationCapacity.spotTypes.find((row) => row.stableType === 'Config6UserSpot');
  const variant = target.populationCapacity.actors.limit === 1
    ? 'short-actor'
    : target.populationCapacity.spots.limit === 1
      ? 'short-spot'
      : spotType?.limit === 1
        ? 'short-type'
        : 'sufficient';
  if (variant === 'short-spot' || variant === 'short-type') {
    const blockerId = `sf-g3-target-blocker-${variant}-${suffix}`;
    const blocker = await postJson<CapacityResult>(options.providerBUrl, '/capacity/spots', {
      spotId: blockerId,
      state: 'blocker'
    });
    ensure(blocker.status === 'created', `SF-G3 ${variant} target blocker was not created.`);
    await waitFor(() => locationsOwnedBy(options, [], blockerId, 'api-b'));
    await waitFor(async () => {
      const row = (await currentMesh(options)).find((candidate) => candidate.rid === 'api-b');
      const type = row?.populationCapacity.spotTypes.find(candidate =>
        candidate.stableType === 'Config6UserSpot');
      return row?.populationCapacity.spots.active === 1
        && (variant !== 'short-type' || type?.active === 1);
    });
  }

  const relocation = await postJsonWithin<{ outcome: number; reason: number }>(
    options.providerAUrl,
    '/drain',
    {},
    40_000
  );
  if (variant === 'sufficient') {
    ensure(relocation.outcome === 0, `SF-G3 sufficient target did not relocate: ${JSON.stringify(relocation)}.`);
    await waitFor(() => locationsOwnedBy(options, actorIds, spotId, 'api-b'));
    await assertAggregate(options.providerBUrl, actorIds, actorStates, spotId, spotState, 'api-b');
  } else {
    ensure(relocation.outcome === 1,
      `SF-G3 ${variant} target did not return a capacity blocker: ${JSON.stringify(relocation)}.`);
    await waitFor(() => locationsOwnedBy(options, actorIds, spotId, 'api-a'));
    await assertAggregate(options.providerAUrl, actorIds, actorStates, spotId, spotState, 'api-a');
  }
  ensure(spot.objectGeneration !== '0', 'SF-G3 source Spot generation is zero.');
  console.log(`scenario SF-G3 variant=${variant} passed`);
}

async function assertAggregate(
  providerUrl: string,
  actorIds: readonly string[],
  states: readonly string[],
  spotId: string,
  spotState: string,
  ownerRid: string
): Promise<void> {
  for (let index = 0; index < actorIds.length; index += 1) {
    const probe = await postJson<ProbeResult>(providerUrl, '/capacity/actors/probe', { actorId: actorIds[index] });
    ensure(probe.actorId === actorIds[index], 'SF-G3 probe returned another Actor.');
    ensure(probe.actorState === states[index], 'SF-G3 changed Actor state.');
    ensure(probe.spotId === spotId && probe.spotState === spotState, 'SF-G3 changed User Spot state.');
    ensure(probe.nodeRid === ownerRid, `SF-G3 expected owner ${ownerRid}, got ${probe.nodeRid}.`);
  }
}

async function locationsOwnedBy(
  options: ClientOptions,
  actorIds: readonly string[],
  spotId: string,
  ownerRid: string
): Promise<boolean> {
  const rows = await Promise.all([
    ...actorIds.map((actorId) => getJson<LocationResult>(
      options.consumerUrl,
      `/location/object?kind=actor&id=${encodeURIComponent(actorId)}`
    )),
    getJson<LocationResult>(
      options.consumerUrl,
      `/location/object?kind=spot&id=${encodeURIComponent(spotId)}`
    )
  ]);
  return rows.every((row) => row.found && row.ownerNodeRid === ownerRid && row.objectGeneration !== '0');
}

async function waitForProviderB(options: ClientOptions): Promise<MeshRow> {
  let found: MeshRow | undefined;
  await waitFor(async () => {
    try {
      await getJson(options.providerBUrl!, '/health');
      found = (await currentMesh(options)).find((row) => row.rid === 'api-b');
      return found !== undefined;
    } catch {
      return false;
    }
  });
  return found!;
}

async function currentMesh(options: ClientOptions): Promise<readonly MeshRow[]> {
  return await getJson(options.consumerUrl, '/location/mesh');
}

async function waitFor(check: () => Promise<boolean>, timeoutMs = 15_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    if (await check()) return;
    await new Promise((resolve) => setTimeout(resolve, 50));
  }
  throw new Error('SF-G3 timed out waiting for the public state transition.');
}
