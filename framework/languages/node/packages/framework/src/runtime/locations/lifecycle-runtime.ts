import {
  type ZLinkActorLocation,
  type ZLinkActorLocationKey,
  ZLinkLocationKind,
  ZLinkLocationWriteIntent,
  type ZLinkLocationWriteResult,
  type ZLinkLocationWriteStatus,
  type ZLinkRouteLocation,
  type ZLinkRouteLocationKey,
  type ZLinkSpotLocation,
  type ZLinkSpotLocationKey
} from './internal-location-contracts';
import { routingIdsEqual } from '../routing-id';
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations/Writes';

export interface ZLinkOwnershipLostEvent {
  readonly kind: ZLinkLocationKind;
  readonly key: string;
}

export type ZLinkDiscardedAuthorityFence = Pick<
  import('./internal-location-contracts').ZLinkAuthoritySnapshot,
  'ownerId' | 'ownerLeaseGeneration' | 'objectGeneration' | 'authorityOwnerGeneration'
> & {
  readonly allocation: Pick<
    import('./internal-location-contracts').ZLinkAuthoritySnapshot['allocation'],
    'descriptor' | 'descriptorLifecycleGeneration'
  >;
};

export function sameAuthorityIncarnation(
  current: import('./internal-location-contracts').ZLinkAuthoritySnapshot,
  fence: ZLinkDiscardedAuthorityFence
): boolean {
  return (
    current.allocation.state === 'active' &&
    current.ownerId === fence.ownerId &&
    current.objectGeneration === fence.objectGeneration &&
    current.authorityOwnerGeneration === fence.authorityOwnerGeneration &&
    current.allocation.descriptor.meshName === fence.allocation.descriptor.meshName &&
    routingIdsEqual(current.allocation.descriptor.rid, fence.allocation.descriptor.rid) &&
    current.allocation.descriptorLifecycleGeneration ===
      fence.allocation.descriptorLifecycleGeneration
  );
}

export interface ZLinkDiscardedAuthorityStore {
  cleanupDiscardedAuthority?(
    key: import('./internal-location-contracts').ZLinkAuthorityKey,
    fence: ZLinkDiscardedAuthorityFence,
    owner: ZLinkLocationOwnerToken,
    signal?: AbortSignal
  ): Promise<boolean>;
}

export interface ZLinkExecutionRecovery {
  readonly key: import('./internal-location-contracts').ZLinkAuthorityKey;
  readonly meshName: string;
  readonly memberOf?: { readonly spotId: string; readonly objectGeneration: bigint };
  readonly incarnation: Pick<
    import('./internal-location-contracts').ZLinkAuthoritySnapshot,
    'objectGeneration' | 'authorityOwnerGeneration'
  >;
  cleanup(owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<boolean>;
  run(
    owner: ZLinkLocationOwnerToken,
    canSubmit: () => boolean,
    signal?: AbortSignal
  ): Promise<boolean>;
}

export interface IZLinkLocationLifecycleRuntime {
  trackExecution?(
    owner: object,
    steadyReady: () => boolean,
    discard: (error: unknown) => void | Promise<void>,
    captureRecovery?: (reference: WeakRef<object>) => ZLinkExecutionRecovery | undefined
  ): void;
  forgetExecution?(owner: object): void;
  discardExecution?(owner: object): void;
  discardSteadyExecution?(owner: object): boolean;
  readonly ownerId: string;
  readonly currentOwnerToken?: ZLinkLocationOwnerToken;
  reclaimOwnerAuthorities?(publication?: Promise<void>, signal?: AbortSignal): Promise<void>;
  addOwnershipLostHandler(handler: (event: ZLinkOwnershipLostEvent) => void): void;
  removeOwnershipLostHandler(handler: (event: ZLinkOwnershipLostEvent) => void): void;
  writeActor(
    actor: ZLinkActorLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  writeSpot(
    spot: ZLinkSpotLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  writeRoute(
    route: ZLinkRouteLocation,
    intent: ZLinkLocationWriteIntent,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
  removeActor(
    key: ZLinkActorLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus>;
  removeSpot(
    key: ZLinkSpotLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteStatus>;
  removeRoute(
    key: ZLinkRouteLocationKey,
    generation: bigint,
    signal?: AbortSignal
  ): Promise<ZLinkLocationWriteResult>;
}
