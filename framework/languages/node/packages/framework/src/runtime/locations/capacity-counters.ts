import {
  type ZLinkLocationStore,
  type ZLinkMeshNodeDescriptor,
  type ZLinkMeshNodeDescriptorKey,
  type ZLinkStoreCondition,
  type ZLinkStoreMutation,
  ZLinkFrameworkErrorKind,
  ZLinkFrameworkException
} from '../../contracts';
import type { ZLinkCapacityVector } from './internal-location-contracts';
import { storeKey } from './in-memory-provider-location-store';
import { canonicalPlacementObjectKind, routingIdHexSegment } from './opaque-record-key';
import { readStoreCounter } from './provider-write-request';

type CapacityLimits = ZLinkMeshNodeDescriptor['populationCapacity'];

export function capacityScope({ meshName, rid }: ZLinkMeshNodeDescriptorKey): string {
  return `zlink:v12:capacity\0${meshName}\0${routingIdHexSegment(rid)}\0`;
}

function spotTypeDimension(
  type: Pick<NonNullable<ZLinkCapacityVector['spotType']>, 'objectKind' | 'stableType'>
): string {
  return `spot-type\0${canonicalPlacementObjectKind(type.objectKind)}\0${type.stableType}`;
}

function dimensions(capacity: ZLinkCapacityVector, limits?: CapacityLimits) {
  const values: [string, number, number?][] = [
    ['actors', capacity.actors, limits?.actors.limit],
    ['spots', capacity.spots, limits?.spots.limit]
  ];
  const type = capacity.spotType;
  if (type !== undefined) {
    values.push([
      spotTypeDimension(type),
      type.count,
      limits?.spotTypes.find(
        (candidate) =>
          candidate.objectKind === type.objectKind && candidate.stableType === type.stableType
      )?.limit
    ]);
  }
  return values.filter(([, count]) => count !== 0);
}

function capacityCounterSuffix(dimension: string, state: 'active' | 'pending'): string {
  return `${dimension}\0${state}`;
}

interface CapacityChange {
  readonly descriptor: ZLinkMeshNodeDescriptorKey;
  readonly capacity: ZLinkCapacityVector;
  readonly active?: 1 | -1;
  readonly pending?: 1 | -1;
}

export function capacityMutations(...changes: readonly CapacityChange[]): ZLinkStoreMutation[] {
  const deltas = new Map<string, bigint>();
  for (const { descriptor, capacity, active, pending } of changes) {
    const scope = capacityScope(descriptor);
    for (const [dimension, count] of dimensions(capacity)) {
      for (const [state, direction] of [
        ['active', active],
        ['pending', pending]
      ] as const) {
        if (direction === undefined) continue;
        const key = scope + capacityCounterSuffix(dimension, state);
        deltas.set(key, (deltas.get(key) ?? 0n) + BigInt(count) * BigInt(direction));
      }
    }
  }
  return [...deltas].flatMap(([key, delta]) =>
    delta === 0n ? [] : [{ kind: 'add', key: storeKey(key), delta }]
  );
}

export function capacityReservationConditions(
  descriptor: ZLinkMeshNodeDescriptor,
  capacity: ZLinkCapacityVector
): ZLinkStoreCondition[] | undefined {
  const scope = capacityScope(descriptor);
  const conditions: ZLinkStoreCondition[] = [];
  for (const [dimension, count, limit] of dimensions(capacity, descriptor.populationCapacity)) {
    if (limit === undefined) return undefined;
    if (limit !== 0) {
      conditions.push({
        kind: 'sumAtMost',
        keys: [
          storeKey(scope + capacityCounterSuffix(dimension, 'active')),
          storeKey(scope + capacityCounterSuffix(dimension, 'pending'))
        ],
        delta: BigInt(count),
        limit: BigInt(limit)
      });
    }
  }
  return conditions;
}

export async function readCapacity(
  provider: ZLinkLocationStore,
  scope: string,
  signal?: AbortSignal
): Promise<ReadonlyMap<string, number>> {
  const counts = new Map<string, number>();
  let cursor;
  do {
    const page = await provider.scan({ prefix: scope, cursor, limit: 256 }, signal);
    if (page.kind === 'expired') {
      throw new ZLinkFrameworkException(
        ZLinkFrameworkErrorKind.Unavailable,
        'Location Store capacity snapshot expired.'
      );
    }
    for (const { key, value } of page.value.items) {
      const count = readStoreCounter(value.bytes);
      if (count === undefined) {
        throw new ZLinkFrameworkException(
          ZLinkFrameworkErrorKind.DataLost,
          'Location Store capacity counter is not canonical decimal.'
        );
      }
      counts.set(key.value.slice(scope.length), Number(count));
    }
    cursor = page.value.nextCursor;
  } while (cursor !== undefined);
  return counts;
}

export function projectCapacity(
  limits: CapacityLimits,
  counts: ReadonlyMap<string, number>
): CapacityLimits {
  const usage = (dimension: string) => ({
    active: counts.get(capacityCounterSuffix(dimension, 'active')) ?? 0,
    reserved: counts.get(capacityCounterSuffix(dimension, 'pending')) ?? 0
  });
  return {
    actors: { ...limits.actors, ...usage('actors') },
    spots: { ...limits.spots, ...usage('spots') },
    spotTypes: limits.spotTypes.map((type) => ({ ...type, ...usage(spotTypeDimension(type)) }))
  };
}
