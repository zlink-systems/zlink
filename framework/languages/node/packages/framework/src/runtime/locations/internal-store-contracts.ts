import type {
  ZLinkActorLocation,
  ZLinkActorLocationFilter,
  ZLinkActorLocationKey,
  ZLinkLocationOwnerToken,
  ZLinkLocationPage,
  ZLinkLocationWriteIntent,
  ZLinkLocationWriteResult,
  ZLinkLocationWriteStatus,
  ZLinkPageRequest,
  ZLinkPeerLocation,
  ZLinkPeerLocationFilter,
  ZLinkPeerLocationKey,
  ZLinkRouteLocation,
  ZLinkRouteLocationFilter,
  ZLinkRouteLocationKey,
  ZLinkSpotLocation,
  ZLinkSpotLocationFilter,
  ZLinkSpotLocationKey
} from './internal-location-contracts';
import type { ZLinkDomainLocationStore } from './domain-store-contract';

export type ZLinkAuthorityStore = Pick<ZLinkDomainLocationStore,
  | 'readAuthority'
  | 'compareExchangeAuthority'
  | 'listAuthorities'>;

export type ZLinkObjectCreationStore = Pick<ZLinkDomainLocationStore,
  | 'readCreationTerminal'
  | 'reserve'
  | 'commit'
  | 'completeCreation'
  | 'abort'
  | 'prepareAggregate'
  | 'commitAggregate'
  | 'abortAggregate'>;

export type ZLinkRelocationCapacityStore = Pick<ZLinkDomainLocationStore,
  | 'reserveRelocationCapacity'
  | 'abortRelocationCapacity'>;

export type ZLinkOwnerLeaseStore = Pick<ZLinkDomainLocationStore,
  | 'claimOwnerLease'
  | 'readOwnerLease'
  | 'renewOwnerLease'
  | 'releaseOwnerLease'>;

export type ZLinkMeshNodeLocationStore = Pick<ZLinkDomainLocationStore,
  | 'updateMeshNode'
  | 'removeMeshNode'
  | 'listMeshNodes'>;

export type ZLinkClientServerLocationStore = Pick<ZLinkDomainLocationStore,
  | 'updateClientServer'
  | 'removeClientServer'
  | 'listClientServers'>;

export type ZLinkFanoutLocationStore = Pick<ZLinkDomainLocationStore,
  | 'updateFanoutPublisher'
  | 'removeFanoutPublisher'
  | 'listFanoutPublishers'>;

export interface ZLinkPeerLocationStore {
  updatePeer(peer: ZLinkPeerLocation, intent: ZLinkLocationWriteIntent, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  removePeer(key: ZLinkPeerLocationKey, owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  listPeers(filter: ZLinkPeerLocationFilter, signal?: AbortSignal): Promise<readonly ZLinkPeerLocation[]>;
}

export interface ZLinkSpotLocationStore {
  updateSpot(location: ZLinkSpotLocation, intent: ZLinkLocationWriteIntent, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  removeSpot(key: ZLinkSpotLocationKey, owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<ZLinkLocationWriteStatus>;
  resolveSpot(key: ZLinkSpotLocationKey, signal?: AbortSignal): Promise<ZLinkSpotLocation | undefined>;
}

export interface ZLinkSpotLocationQueryStore {
  listSpots(filter: ZLinkSpotLocationFilter, page?: ZLinkPageRequest, signal?: AbortSignal): Promise<ZLinkLocationPage<ZLinkSpotLocation>>;
}

export interface ZLinkActorLocationStore {
  updateActor(location: ZLinkActorLocation, intent: ZLinkLocationWriteIntent, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  removeActor(key: ZLinkActorLocationKey, owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<ZLinkLocationWriteStatus>;
  resolveActor(key: ZLinkActorLocationKey, signal?: AbortSignal): Promise<ZLinkActorLocation | undefined>;
}

export interface ZLinkActorLocationQueryStore {
  listActors(filter: ZLinkActorLocationFilter, page?: ZLinkPageRequest, signal?: AbortSignal): Promise<ZLinkLocationPage<ZLinkActorLocation>>;
}

export interface ZLinkRouteLocationStore {
  updateRoute(route: ZLinkRouteLocation, intent: ZLinkLocationWriteIntent, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  removeRoute(key: ZLinkRouteLocationKey, owner: ZLinkLocationOwnerToken, signal?: AbortSignal): Promise<ZLinkLocationWriteResult>;
  resolveRoute(key: ZLinkRouteLocationKey, signal?: AbortSignal): Promise<ZLinkRouteLocation | undefined>;
  listRoutes(filter: ZLinkRouteLocationFilter, page?: ZLinkPageRequest, signal?: AbortSignal): Promise<ZLinkLocationPage<ZLinkRouteLocation>>;
}
