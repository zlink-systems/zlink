import { ZLINK_MAX_ROUTING_ID_BYTES } from '../../contracts/Common/CoreTypes';
import { completionDataLost } from './relocation-completion-record';

/** Location runtime §2.3, §9.1: the authority record owns durable relocation progress. */
export interface AuthorityRelocationFence {
  readonly nodeRid: string;
  readonly nodeGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
}

export interface AuthorityRelocationCompletion {
  readonly acceptedRequests: bigint;
  readonly completedResults: bigint;
  readonly pendingDeliveries: bigint;
  readonly reference: string | null;
  readonly sha256: string | null;
}

function initialRelocationCompletion(acceptedRequests: bigint): AuthorityRelocationCompletion {
  if (acceptedRequests < 0n || acceptedRequests > 0xffffffffffffffffn)
    completionDataLost('Accepted request count is invalid.');
  return {
    acceptedRequests,
    completedResults: 0n,
    pendingDeliveries: 0n,
    reference: null,
    sha256: null
  };
}

export interface AuthorityRelocation {
  readonly relocationId: string;
  readonly phase:
    | 'preparing'
    | 'captured'
    | 'prepared'
    | 'committed'
    | 'activating'
    | 'activated'
    | 'cleaning'
    | 'completed'
    | 'aborted';
  readonly source: AuthorityRelocationFence;
  readonly target: (AuthorityRelocationFence & { readonly attemptGeneration: bigint }) | null;
  readonly coordinator: AuthorityRelocationFence & { readonly expectedStoreVersion: string };
  readonly sourceInventorySha256: string;
  readonly applicationVersion: bigint;
  readonly completion?: AuthorityRelocationCompletion;
}

export function isPreparedAuthorityRelocation(
  relocation: AuthorityRelocation | undefined,
  aggregateId: string
): relocation is AuthorityRelocation & { readonly phase: 'prepared' } {
  return relocation?.relocationId === aggregateId && relocation.phase === 'prepared';
}

export function commitAuthorityRelocation(
  relocation: AuthorityRelocation | undefined,
  aggregateId: string,
  acceptedRequests = 0n
): AuthorityRelocation | undefined {
  if (!isPreparedAuthorityRelocation(relocation, aggregateId)) return undefined;
  return {
    ...relocation,
    phase: 'committed',
    completion: initialRelocationCompletion(acceptedRequests)
  };
}

export interface AuthorityAggregateMarker {
  readonly aggregateId: string;
  readonly aggregateGeneration: bigint;
  readonly index: number;
  readonly expectedStoreVersion: string;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly targetAuthorityOwnerGeneration: bigint;
  readonly authorityPayloadSha256: string;
  readonly membershipMutationSha256: string;
}

const markerFields = [
  'aggregateId',
  'aggregateGeneration',
  'index',
  'expectedStoreVersion',
  'ownerTransition',
  'targetAuthorityOwnerGeneration',
  'authorityPayloadSha256',
  'membershipMutationSha256'
] as const;
const phases = new Set([
  'preparing',
  'captured',
  'prepared',
  'committed',
  'activating',
  'activated',
  'cleaning',
  'completed',
  'aborted'
]);

export function encodeAuthorityAggregate(
  marker: AuthorityAggregateMarker | undefined,
  relocation: AuthorityRelocation | undefined
): Record<string, unknown> | undefined {
  if (marker === undefined && relocation === undefined) return undefined;
  const value = JSON.parse(
    JSON.stringify(
      { ...marker, ...(relocation === undefined ? {} : { relocation }) },
      (_key, item: unknown) => (typeof item === 'bigint' ? item.toString() : item)
    )
  ) as Record<string, unknown>;
  decodeAuthorityAggregate(value);
  return value;
}

export function decodeAuthorityAggregate(value: unknown): {
  marker: AuthorityAggregateMarker | undefined;
  relocation: AuthorityRelocation | undefined;
} {
  if (value === undefined) return { marker: undefined, relocation: undefined };
  const aggregate = object(value);
  const present = markerFields.filter((field) => aggregate[field] !== undefined);
  if (
    (present.length !== 0 && present.length !== markerFields.length) ||
    (present.length === 0 && aggregate.relocation === undefined)
  )
    throw new TypeError('Authority aggregate marker must be complete or absent.');
  const marker =
    present.length === 0
      ? undefined
      : {
          aggregateId: text(aggregate.aggregateId),
          aggregateGeneration: integer(aggregate.aggregateGeneration),
          index: aggregate.index as number,
          expectedStoreVersion: text(aggregate.expectedStoreVersion),
          ownerTransition: aggregate.ownerTransition as AuthorityAggregateMarker['ownerTransition'],
          targetAuthorityOwnerGeneration: integer(aggregate.targetAuthorityOwnerGeneration),
          authorityPayloadSha256: text(aggregate.authorityPayloadSha256),
          membershipMutationSha256: text(aggregate.membershipMutationSha256)
        };
  if (aggregate.relocation === undefined) return { marker, relocation: undefined };
  const state = object(aggregate.relocation);
  const phase = text(state.phase) as AuthorityRelocation['phase'];
  if (
    !phases.has(phase) ||
    !/^[0-9a-f]{8}-(?:[0-9a-f]{4}-){3}[0-9a-f]{12}$/.test(text(state.relocationId)) ||
    !/^[0-9a-f]{64}$/.test(text(state.sourceInventorySha256))
  )
    throw new TypeError('Authority relocation identity or phase is invalid.');
  const target =
    state.target === null
      ? null
      : {
          ...fence(state.target),
          attemptGeneration: nonzeroInteger(object(state.target).attemptGeneration)
        };
  if (
    ((phase === 'preparing' || phase === 'captured') && target !== null) ||
    (phase !== 'preparing' && phase !== 'captured' && phase !== 'aborted' && target === null)
  )
    throw new TypeError('Authority relocation target does not match its phase.');
  return {
    marker,
    relocation: {
      relocationId: text(state.relocationId),
      phase,
      source: fence(state.source),
      target,
      coordinator: {
        ...fence(state.coordinator),
        expectedStoreVersion: text(object(state.coordinator).expectedStoreVersion)
      },
      sourceInventorySha256: text(state.sourceInventorySha256),
      applicationVersion: integer(state.applicationVersion),
      ...(state.completion === undefined
        ? {}
        : { completion: decodeAuthorityRelocationCompletion(state.completion) })
    }
  };
}

function object(value: unknown): Record<string, unknown> {
  if (value === null || typeof value !== 'object' || Array.isArray(value))
    throw new TypeError('Authority relocation object is invalid.');
  return value as Record<string, unknown>;
}
function text(value: unknown): string {
  if (typeof value !== 'string' || value.length === 0)
    throw new TypeError('Authority relocation text is invalid.');
  return value;
}
function integer(value: unknown): bigint {
  if (
    typeof value !== 'string' ||
    !/^(0|[1-9][0-9]*)$/.test(value) ||
    BigInt(value) > 0xffffffffffffffffn
  )
    throw new TypeError('Authority relocation integer must be a decimal uint64 string.');
  return BigInt(value);
}
function nonzeroInteger(value: unknown): bigint {
  const result = integer(value);
  if (result === 0n) throw new TypeError('Authority relocation fence generation must be nonzero.');
  return result;
}

function fence(value: unknown): AuthorityRelocationFence {
  const item = object(value);
  const nodeRid = text(item.nodeRid);
  if (
    nodeRid.length > 2 * ZLINK_MAX_ROUTING_ID_BYTES ||
    nodeRid.length % 2 !== 0 ||
    /[^0-9a-f]/.test(nodeRid)
  )
    throw new TypeError('Authority relocation node RID must be canonical hex.');
  return {
    nodeRid,
    nodeGeneration: nonzeroInteger(item.nodeGeneration),
    ownerId: text(item.ownerId),
    ownerLeaseGeneration: nonzeroInteger(item.ownerLeaseGeneration)
  };
}

export function decodeAuthorityRelocationCompletion(value: unknown): AuthorityRelocationCompletion {
  const item = object(value);
  const completion = {
    acceptedRequests: integer(item.acceptedRequests),
    completedResults: integer(item.completedResults),
    pendingDeliveries: integer(item.pendingDeliveries),
    reference: item.reference === null ? null : text(item.reference),
    sha256: item.sha256 === null ? null : text(item.sha256)
  };
  if (
    completion.completedResults > completion.acceptedRequests ||
    completion.pendingDeliveries > completion.completedResults ||
    (completion.reference === null) !== (completion.sha256 === null) ||
    (completion.reference === null) !== (completion.completedResults === 0n) ||
    (completion.sha256 !== null && !/^[0-9a-f]{64}$/.test(completion.sha256))
  )
    completionDataLost('Invalid completion counts or reference.');
  return completion;
}
