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

export interface IZLinkLocationLifecycleRuntime {
  readonly ownerId: string;
  readonly currentOwnerToken?: ZLinkLocationOwnerToken;
  reclaimOwnerAuthorities?(signal?: AbortSignal): Promise<void>;
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
  removeActor(key: ZLinkActorLocationKey, generation: bigint, signal?: AbortSignal): Promise<ZLinkLocationWriteStatus>;
  removeSpot(key: ZLinkSpotLocationKey, generation: bigint, signal?: AbortSignal): Promise<ZLinkLocationWriteStatus>;
  removeRoute(key: ZLinkRouteLocationKey, generation: bigint, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
}
