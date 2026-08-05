import type { RoutingId } from '../Common';
import type { ZLinkMeshNodeDescriptorKey } from './Keys';
import type { ZLinkLocationOwnerToken } from './Writes';

declare const zlinkAuthorityKeyBrand: unique symbol;
declare const zlinkAuthorityVersionBrand: unique symbol;
declare const zlinkRelocationCapacityFenceBrand: unique symbol;
declare const zlinkAggregateIdBrand: unique symbol;

export interface ZLinkAuthorityKey {
  readonly value: string;
  readonly [zlinkAuthorityKeyBrand]: true;
}

export interface ZLinkAuthorityStoreVersion {
  readonly value: string;
  readonly [zlinkAuthorityVersionBrand]: true;
}

export type ZLinkPlacementObjectKind = 'actor' | 'user_spot' | 'instance_spot';
export type ZLinkPlacementAllocationState = 'reserved' | 'active';

export interface ZLinkSpotTypeCapacityDelta {
  readonly objectKind: 'user_spot' | 'instance_spot';
  readonly stableType: string;
  readonly count: number;
}

export interface ZLinkCapacityVector {
  readonly actors: number;
  readonly spots: number;
  readonly spotType?: ZLinkSpotTypeCapacityDelta;
}

export interface ZLinkPlacementAllocation {
  readonly state: ZLinkPlacementAllocationState;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType: string;
  readonly descriptor: ZLinkMeshNodeDescriptorKey;
  readonly descriptorLifecycleGeneration: bigint;
  readonly capacity: ZLinkCapacityVector;
}

export interface ZLinkPendingObjectCreation {
  readonly reservationId: string;
  readonly requestContentReference: string;
  readonly requestSha256: Uint8Array;
  readonly requestEncodedSize: bigint;
}

/** Immutable creation reservation projected by an authority snapshot. */
export interface ZLinkReservedObjectCreation extends ZLinkPendingObjectCreation {}

export interface ZLinkAuthoritySnapshot {
  readonly kind: 'snapshot';
  readonly storeVersion: ZLinkAuthorityStoreVersion;
  readonly payload: Uint8Array;
  readonly objectGeneration: bigint;
  readonly authorityOwnerGeneration: bigint;
  readonly ownerId: string;
  readonly ownerLeaseGeneration: bigint;
  readonly allocation: ZLinkPlacementAllocation;
  readonly pendingCreation?: ZLinkPendingObjectCreation;
  readonly storeNow: Date;
}

export type ZLinkAuthorityReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | ZLinkAuthoritySnapshot;

export interface ZLinkRelocationCapacityFence {
  readonly value: string;
  readonly [zlinkRelocationCapacityFenceBrand]: true;
}

export type ZLinkAuthorityMutation =
  | {
      readonly kind: 'put';
      readonly payload: Uint8Array;
      readonly generationTransition: 'preserve' | 'newOwner';
      readonly targetOwner?: ZLinkLocationOwnerToken;
      readonly relocationCapacityFence?: ZLinkRelocationCapacityFence;
    }
  | {
      /** Rebinds an authority retained by the same owner after lease recovery. */
      readonly kind: 'rebindOwnerLease';
      readonly payload: Uint8Array;
      readonly expectedOwner: ZLinkLocationOwnerToken;
      readonly targetOwner: ZLinkLocationOwnerToken;
    }
  | {
      readonly kind: 'restore';
      readonly payload: Uint8Array;
      readonly expectedOwner: ZLinkLocationOwnerToken;
    }
  | { readonly kind: 'delete' };

export type ZLinkAuthorityCompareExchangeResult =
  | ({ readonly kind: 'stored' } & Omit<ZLinkAuthoritySnapshot, 'kind'>)
  | {
      readonly kind: 'deleted';
      readonly storeVersion: ZLinkAuthorityStoreVersion;
      readonly storeNow: Date;
    }
  | { readonly kind: 'conflict'; readonly current: ZLinkAuthorityReadResult }
  | { readonly kind: 'generationExhausted' };

export interface ZLinkAuthorityEntry {
  readonly key: ZLinkAuthorityKey;
  readonly snapshot: ZLinkAuthoritySnapshot;
}

export class ZLinkAuthorityScanCursor {
  private constructor(readonly encoded: string) {}

  static from(encoded: string): ZLinkAuthorityScanCursor {
    const byteLength = Buffer.byteLength(encoded, 'utf8');
    if (byteLength < 1 || byteLength > 4096) {
      throw new RangeError('Authority scan cursor must contain 1..4096 UTF-8 bytes.');
    }
    return new ZLinkAuthorityScanCursor(encoded);
  }
}

export interface ZLinkAuthorityPage {
  readonly kind: 'page';
  readonly items: readonly ZLinkAuthorityEntry[];
  readonly nextCursor?: ZLinkAuthorityScanCursor;
}

export type ZLinkAuthorityScanResult =
  | ZLinkAuthorityPage
  | { readonly kind: 'scanExpired' };

export interface ZLinkObjectCreationKey {
  readonly kind: ZLinkPlacementObjectKind;
  readonly globalId: string;
}

export interface ZLinkObjectCreationTarget {
  readonly meshName: string;
  readonly nodeRid: RoutingId;
  readonly nodeLifecycleGeneration: bigint;
  readonly owner: ZLinkLocationOwnerToken;
}

export interface ZLinkObjectCreationIntent {
  readonly stableType: string;
  readonly requestContentReference: string;
  readonly requestSha256: Uint8Array;
  readonly requestEncodedSize: bigint;
}

export interface ZLinkCreationOperationId {
  readonly high: bigint;
  readonly low: bigint;
}

export interface ZLinkCreationOperationIdentity {
  readonly sourceNodeRid: RoutingId;
  readonly sourceNodeGeneration: bigint;
  readonly operationId: ZLinkCreationOperationId;
}

export type ZLinkCreationTerminalState = 'created' | 'rejected' | 'failed';

export interface ZLinkCreationTerminalPublication {
  readonly operation: ZLinkCreationOperationIdentity;
  readonly terminalEnvelope: Uint8Array;
  readonly terminalEnvelopeSha256: Uint8Array;
  /**
   * The original operation deadline. Providers retain the terminal for five
   * minutes after this absolute instant.
   */
  readonly operationDeadline: Date;
}

export interface ZLinkCreationTerminalRecord {
  readonly state: ZLinkCreationTerminalState;
  readonly operation: ZLinkCreationOperationIdentity;
  readonly reservationId: string;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly terminalEnvelope: Uint8Array;
  readonly terminalEnvelopeSha256: Uint8Array;
  readonly expiresAt: Date;
  readonly storeNow: Date;
}

export type ZLinkCreationTerminalReadResult =
  | { readonly kind: 'missing'; readonly storeNow: Date }
  | { readonly kind: 'found'; readonly record: ZLinkCreationTerminalRecord };

export interface ZLinkObjectReserveRequest {
  readonly key: ZLinkObjectCreationKey;
  readonly intent: ZLinkObjectCreationIntent;
  readonly target: ZLinkObjectCreationTarget;
  readonly creatingPayload: Uint8Array;
  readonly capacity: ZLinkCapacityVector;
}

export type ZLinkObjectReserveResult =
  | {
      readonly kind: 'reserved';
      readonly reservationId: string;
      readonly creating: ZLinkAuthoritySnapshot;
    }
  | { readonly kind: 'alreadyExists'; readonly current: ZLinkAuthoritySnapshot }
  | { readonly kind: 'typeMismatch'; readonly current: ZLinkAuthoritySnapshot }
  | { readonly kind: 'placementCapacityExhausted' }
  | { readonly kind: 'conflict'; readonly current: ZLinkAuthorityReadResult }
  | { readonly kind: 'generationExhausted' };

export interface ZLinkObjectCommitRequest {
  readonly key: ZLinkObjectCreationKey;
  readonly reservationId: string;
  readonly expectedStoreVersion: string;
  readonly target: ZLinkObjectCreationTarget;
  readonly readyPayload: Uint8Array;
}

export type ZLinkObjectCommitResult =
  | { readonly kind: 'committed'; readonly ready: ZLinkAuthoritySnapshot }
  | { readonly kind: 'alreadyCommitted'; readonly ready: ZLinkAuthoritySnapshot }
  | { readonly kind: 'stale' }
  | { readonly kind: 'generationExhausted' };

export type ZLinkObjectCreationCompletion =
  | {
      readonly kind: 'created';
      readonly readyPayload: Uint8Array;
      readonly terminal: ZLinkCreationTerminalPublication;
    }
  | {
      readonly kind: 'rejected' | 'failed';
      readonly terminal: ZLinkCreationTerminalPublication;
    };

export interface ZLinkObjectCreationCompleteRequest {
  readonly key: ZLinkObjectCreationKey;
  readonly reservationId: string;
  readonly expectedStoreVersion: string;
  readonly target: ZLinkObjectCreationTarget;
  readonly completion: ZLinkObjectCreationCompletion;
}

export type ZLinkObjectCreationCompleteResult =
  | {
      readonly kind: 'created';
      readonly ready: ZLinkAuthoritySnapshot;
      readonly terminal: ZLinkCreationTerminalRecord;
    }
  | {
      readonly kind: 'rejected' | 'failed' | 'alreadyCompleted';
      readonly terminal: ZLinkCreationTerminalRecord;
    }
  | { readonly kind: 'stale' }
  | { readonly kind: 'generationExhausted' };

export interface ZLinkObjectAbortRequest {
  readonly key: ZLinkObjectCreationKey;
  readonly reservationId: string;
  readonly expectedStoreVersion: string;
  readonly target: ZLinkObjectCreationTarget;
}

export type ZLinkObjectAbortResult =
  | { readonly kind: 'aborted' }
  | { readonly kind: 'alreadyAborted' }
  | { readonly kind: 'stale' };

export interface ZLinkRelocationCapacityReservationRequest {
  readonly reservationId: string;
  readonly authorityKey: ZLinkAuthorityKey;
  readonly expectedStoreVersion: ZLinkAuthorityStoreVersion;
  readonly objectKind: ZLinkPlacementObjectKind;
  readonly stableType: string;
  readonly sourceDescriptor: ZLinkMeshNodeDescriptorKey;
  readonly sourceNodeLifecycleGeneration: bigint;
  readonly sourceOwner: ZLinkLocationOwnerToken;
  readonly targetDescriptor: ZLinkMeshNodeDescriptorKey;
  readonly targetNodeLifecycleGeneration: bigint;
  readonly targetOwner: ZLinkLocationOwnerToken;
  readonly capacity: ZLinkCapacityVector;
}

export type ZLinkRelocationCapacityReserveResult =
  | { readonly kind: 'reserved'; readonly fence: ZLinkRelocationCapacityFence }
  | { readonly kind: 'alreadyReserved'; readonly fence: ZLinkRelocationCapacityFence }
  | { readonly kind: 'conflict'; readonly current: ZLinkAuthorityReadResult }
  | { readonly kind: 'targetUnavailable' }
  | { readonly kind: 'placementCapacityExhausted' };

export type ZLinkRelocationCapacityAbortResult =
  | 'aborted'
  | 'alreadyAborted'
  | 'alreadyCommitted'
  | 'stale';

export interface ZLinkAggregateId {
  readonly value: string;
  readonly [zlinkAggregateIdBrand]: true;
}

export interface ZLinkAggregateParticipant {
  readonly authorityKey: ZLinkAuthorityKey;
  readonly expectedStoreVersion: ZLinkAuthorityStoreVersion;
  readonly ownerTransition: 'preserve' | 'newOwner';
  readonly authorityPayload: Uint8Array;
  readonly membershipMutation: Uint8Array;
}

export interface ZLinkAggregatePrepareRequest {
  readonly aggregateId: ZLinkAggregateId;
  readonly aggregateGeneration: bigint;
  readonly participants: readonly ZLinkAggregateParticipant[];
  readonly inventoryDigest: Uint8Array;
  readonly targetDescriptor: ZLinkMeshNodeDescriptorKey;
  readonly targetDescriptorLifecycleGeneration: bigint;
  readonly capacity: ZLinkCapacityVector;
  readonly targetOwner: ZLinkLocationOwnerToken;
}

export interface ZLinkAggregateFence {
  readonly aggregateId: ZLinkAggregateId;
  readonly aggregateGeneration: bigint;
}

export type ZLinkAggregatePrepareResult =
  | { readonly kind: 'prepared'; readonly fence: ZLinkAggregateFence }
  | { readonly kind: 'alreadyPrepared'; readonly fence: ZLinkAggregateFence }
  | { readonly kind: 'conflict' }
  | { readonly kind: 'stale' }
  | { readonly kind: 'generationExhausted' };

export type ZLinkAggregateCommitResult =
  | { readonly kind: 'committed' }
  | { readonly kind: 'alreadyCommitted' }
  | { readonly kind: 'stale' }
  | { readonly kind: 'generationExhausted' };

export type ZLinkAggregateAbortResult =
  | { readonly kind: 'aborted' }
  | { readonly kind: 'alreadyAborted' }
  | { readonly kind: 'stale' };
