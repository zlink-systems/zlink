import type { ZLinkLocationKey } from './Keys';
import type { ZLinkLocationKind, ZLinkRouteKind } from './Values';

export interface ZLinkLocationWatchFilter {
  readonly kind: ZLinkLocationKind;
  readonly meshName?: string;
  readonly routeKind?: ZLinkRouteKind;
}

export enum ZLinkLocationChangeType {
  Upserted = 'upserted',
  Removed = 'removed',
  Expired = 'expired'
}

export enum ZLinkLocationChangeScopeKind {
  MeshNode = 'meshNode',
  ClientServer = 'clientServer',
  Spot = 'spot',
  Authority = 'authority',
  OwnerLease = 'ownerLease',
  FanoutPublisher = 'fanoutPublisher'
}

export interface ZLinkLocationChanged {
  readonly kind: ZLinkLocationKind;
  readonly key: ZLinkLocationKey;
  readonly changeType: ZLinkLocationChangeType;
  readonly generation: bigint;
  readonly updatedAt: Date;
}

export interface ZLinkLocationChangeStampScope {
  readonly kind: ZLinkLocationKind | ZLinkLocationChangeScopeKind;
  readonly meshName?: string;
  readonly channelName?: string;
}
