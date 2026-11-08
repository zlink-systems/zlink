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
import type { ZLinkLocationOwnerToken } from '../../contracts/Locations/Writes';

export interface ZLinkOwnershipLostEvent {
  readonly kind: ZLinkLocationKind;
  readonly key: string;
}

export type ZLinkPreservedAuthority = (
  key: import('./internal-location-contracts').ZLinkAuthorityKey,
  incarnation?: Pick<
    import('./internal-location-contracts').ZLinkAuthoritySnapshot,
    'objectGeneration' | 'authorityOwnerGeneration'
  >
) => boolean;

export type ZLinkAuthorityConfirmation = (
  key: import('./internal-location-contracts').ZLinkAuthorityKey,
  expected: import('./internal-location-contracts').ZLinkAuthoritySnapshot,
  confirmed: import('./internal-location-contracts').ZLinkAuthoritySnapshot
) => void;

export interface ZLinkExecutionRecovery {
  readonly key: import('./internal-location-contracts').ZLinkAuthorityKey;
  readonly meshName: string;
  readonly incarnation: Pick<
    import('./internal-location-contracts').ZLinkAuthoritySnapshot,
    'objectGeneration' | 'authorityOwnerGeneration'
  >;
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
    discard: (error: unknown) => void,
    captureRecovery?: (reference: WeakRef<object>) => ZLinkExecutionRecovery | undefined
  ): void;
  forgetExecution?(owner: object): void;
  discardExecution?(owner: object): void;
  discardSteadyExecution?(owner: object): boolean;
  readonly ownerId: string;
  readonly currentOwnerToken?: ZLinkLocationOwnerToken;
  reclaimOwnerAuthorities?(
    publication?: Promise<void>,
    signal?: AbortSignal,
    onConfirmed?: ZLinkAuthorityConfirmation
  ): Promise<ZLinkPreservedAuthority>;
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
