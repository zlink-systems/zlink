// SF-G1: Actor·Spot·stable type limit을 atomic하게 적용한다 시나리오를 검증한다.
import type { ClientOptions } from '../Support/client-options';
import { getJson, postJson } from '../../../http-client';
import { ensure } from '../Support/scenario-assert';

interface MeshRow {
  readonly rid: string;
  readonly populationCapacity: {
    readonly actors: { readonly limit: number; readonly active: number };
    readonly spots: { readonly limit: number; readonly active: number };
    readonly spotTypes: readonly { readonly stableType: string; readonly limit: number; readonly active: number }[];
  };
}

interface CapacityRes {
  readonly status: string;
  readonly errorKind?: string;
  readonly actorId?: string;
  readonly spotId?: string;
}

export async function runSFG1(options: ClientOptions): Promise<void> {
  const provider = await providerCapacity(options);
  ensure(provider.populationCapacity.actors.limit === 3, 'SF-G1 Actor limit is not 3.');
  ensure(provider.populationCapacity.spots.limit === 4, 'SF-G1 Spot limit is not 4.');
  ensure(provider.populationCapacity.spotTypes.some((row) =>
    row.stableType === 'Config6UserSpot' && row.limit === 3),
  'SF-G1 User Spot stable type limit is not 3.');

  const failedActor = await createActor(options, 'sf-g1-actor-factory-fail');
  ensure(failedActor.status === 'error' && failedActor.errorKind !== 'CapacityExceeded',
    'SF-G1 Actor factory failure was not returned as an application failure.');

  const actorResults = await Promise.all(
    Array.from({ length: 4 }, (_, index) => createActor(options, `sf-g1-actor-${index}`))
  );
  const createdActors = actorResults.filter((result) => result.status === 'created');
  const blockedActors = actorResults.filter((result) => result.errorKind === 'CapacityExceeded');
  ensure(createdActors.length === 3 && blockedActors.length === 1,
    `SF-G1 expected 3 Actor creates and 1 CapacityExceeded, got ${createdActors.length}/${blockedActors.length}: ${JSON.stringify(actorResults)}.`);

  await postJson(options.providerAUrl, '/capacity/actors/destroy', { actorId: createdActors[0].actorId });
  const reusedActor = await createActor(options, 'sf-g1-actor-reused');
  ensure(reusedActor.status === 'created', 'SF-G1 did not reuse an Actor slot after cleanup.');
  await Promise.all(
    [...createdActors.slice(1), reusedActor].map((result) =>
      postJson(options.providerAUrl, '/capacity/actors/destroy', { actorId: result.actorId }))
  );

  const failedSpot = await createSpot(options, 'sf-g1-spot-factory-fail', true);
  ensure(failedSpot.status === 'error' && failedSpot.errorKind !== 'CapacityExceeded',
    'SF-G1 User Spot factory failure was not returned as an application failure.');
  const spotResults = await Promise.all(
    Array.from({ length: 4 }, (_, index) => createSpot(options, `sf-g1-user-spot-${index}`, false))
  );
  const createdSpots = spotResults.filter((result) => result.status !== 'error');
  const blockedSpots = spotResults.filter((result) => result.errorKind === 'CapacityExceeded');
  ensure(createdSpots.length === 3 && blockedSpots.length === 1,
    `SF-G1 expected 3 User Spot creates and 1 CapacityExceeded, got ${createdSpots.length}/${blockedSpots.length}.`);

  await postJson(options.providerAUrl, '/capacity/spots/close', { spotId: createdSpots[0].spotId });
  const reusedSpot = await createSpot(options, 'sf-g1-user-spot-reused', false);
  ensure(reusedSpot.status !== 'error', 'SF-G1 did not reuse a User Spot slot after cleanup.');
  await Promise.all(
    [...createdSpots.slice(1), reusedSpot].map((result) =>
      postJson(options.providerAUrl, '/capacity/spots/close', { spotId: result.spotId }))
  );

  const after = await providerCapacity(options);
  ensure(after.populationCapacity.actors.active <= after.populationCapacity.actors.limit,
    'SF-G1 Actor active count exceeded its limit.');
  ensure(after.populationCapacity.spots.active <= after.populationCapacity.spots.limit,
    'SF-G1 Spot active count exceeded its limit.');
  ensure(after.populationCapacity.spotTypes.every((row) => row.limit === 0 || row.active <= row.limit),
    'SF-G1 stable type active count exceeded its limit.');
  console.log('scenario SF-G1 passed');
}

async function providerCapacity(options: ClientOptions): Promise<MeshRow> {
  const mesh = await getJson<readonly MeshRow[]>(options.consumerUrl, '/location/mesh');
  const provider = mesh.find((row) => row.rid === 'api-a');
  ensure(provider !== undefined, 'SF-G1 provider descriptor is missing.');
  return provider;
}

async function createActor(options: ClientOptions, actorId: string): Promise<CapacityRes> {
  return await postJson(options.providerAUrl, '/capacity/actors', { actorId });
}

async function createSpot(options: ClientOptions, spotId: string, failFactory: boolean): Promise<CapacityRes> {
  return await postJson(options.providerAUrl, '/capacity/spots', { spotId, failFactory });
}
