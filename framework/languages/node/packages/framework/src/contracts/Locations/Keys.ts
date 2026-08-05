import type { RoutingId, SpotId } from '../Common';
import type { ZLinkSpotKind } from '../Spots';
import type { ZLinkLocationAutoConnectType, ZLinkLocationKind, ZLinkLocationRole, ZLinkRouteKind } from './Values';

/**
 * Peer identity is the full five-component tuple. Missing optional components
 * are still part of the identity as "no value".
 */
export interface ZLinkPeerLocationKey {
  readonly autoConnectType: ZLinkLocationAutoConnectType;
  readonly meshName: string;
  readonly role: ZLinkLocationRole;
  readonly nodeRid?: RoutingId;
  readonly endpoint?: string;
}

export interface ZLinkSpotLocationKey {
  readonly meshName: string;
  readonly spotId: SpotId;
}

export interface ZLinkMeshNodeDescriptorKey {
  readonly meshName: string;
  readonly rid: RoutingId;
}

export interface ZLinkClientServerServerDescriptorKey {
  readonly channelName: string;
  readonly serverRid: RoutingId;
}

export interface ZLinkFanoutPublisherDescriptorKey {
  readonly channelName: string;
  readonly publisherRid: RoutingId;
}

export interface ZLinkActorLocationKey {
  readonly meshName: string;
  readonly actorId: string;
}

export interface ZLinkRouteLocationKey {
  readonly routeKind: ZLinkRouteKind;
  readonly routeKey: string;
}

export type ZLinkLocationKey =
  | { readonly kind: ZLinkLocationKind.Peer; readonly key: ZLinkPeerLocationKey }
  | { readonly kind: ZLinkLocationKind.Spot; readonly key: ZLinkSpotLocationKey }
  | { readonly kind: ZLinkLocationKind.Actor; readonly key: ZLinkActorLocationKey }
  | { readonly kind: ZLinkLocationKind.Route; readonly key: ZLinkRouteLocationKey }
  | {
      readonly kind: ZLinkLocationKind.ClientServer;
      readonly key: ZLinkClientServerServerDescriptorKey;
    };

export interface ZLinkPeerLocationFilter {
  readonly autoConnectType?: ZLinkLocationAutoConnectType;
  readonly meshName?: string;
  readonly role?: ZLinkLocationRole;
  readonly nodeRid?: RoutingId;
  readonly endpoint?: string;
}

export interface ZLinkSpotLocationFilter {
  readonly meshName?: string;
  readonly spotType?: string;
  readonly nodeRid?: RoutingId;
  readonly spotKind?: ZLinkSpotKind;
}

export interface ZLinkActorLocationFilter {
  readonly actorType?: string;
  readonly nodeRid?: RoutingId;
  readonly spotId?: SpotId;
  readonly locationKind?: ZLinkSpotKind;
}

export interface ZLinkRouteLocationFilter {
  readonly routeKind?: ZLinkRouteKind;
  readonly ownerNodeRid?: RoutingId;
  readonly ownerId?: string;
}

export interface ZLinkPageRequest {
  readonly pageSize?: number;
  readonly continuationToken?: string;
}

export interface ZLinkLocationPage<T> {
  readonly items: readonly T[];
  readonly continuationToken?: string;
}
