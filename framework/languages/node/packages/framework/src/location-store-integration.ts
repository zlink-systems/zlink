import type {
  ZLinkActorLocation,
  ZLinkActorLocationFilter,
  ZLinkPeerLocation,
  ZLinkPeerLocationFilter,
  ZLinkRouteLocation,
  ZLinkRouteLocationFilter,
  ZLinkSpotLocation,
  ZLinkSpotLocationFilter
} from './runtime/locations/internal-location-contracts';
import { routingIdsEqual } from './runtime/routing-id';

export function matchesPeerLocation(
  row: ZLinkPeerLocation,
  filter: ZLinkPeerLocationFilter
): boolean {
  return (filter.autoConnectType === undefined || row.autoConnectType === filter.autoConnectType)
    && (filter.meshName === undefined || row.meshName === filter.meshName)
    && (filter.role === undefined || row.role === filter.role)
    && (filter.nodeRid === undefined || routingIdsEqual(row.nodeRid, filter.nodeRid))
    && (filter.endpoint === undefined || row.endpoint === filter.endpoint);
}

export function matchesSpotLocation(
  row: ZLinkSpotLocation,
  filter: ZLinkSpotLocationFilter
): boolean {
  return (filter.meshName === undefined || row.meshName === filter.meshName)
    && (filter.spotType === undefined || row.spotType === filter.spotType)
    && (filter.nodeRid === undefined || routingIdsEqual(row.ownerNodeRid, filter.nodeRid))
    && (filter.spotKind === undefined || row.spotKind === filter.spotKind);
}

export function matchesActorLocation(
  row: ZLinkActorLocation,
  filter: ZLinkActorLocationFilter
): boolean {
  return (filter.actorType === undefined || row.actorType === filter.actorType)
    && (filter.nodeRid === undefined || routingIdsEqual(row.ownerNodeRid, filter.nodeRid))
    && (filter.spotId === undefined || row.spotId === filter.spotId)
    && (filter.locationKind === undefined || row.spotKind === filter.locationKind);
}

export function matchesRouteLocation(
  row: ZLinkRouteLocation,
  filter: ZLinkRouteLocationFilter
): boolean {
  return (filter.routeKind === undefined || row.routeKind === filter.routeKind)
    && (filter.ownerNodeRid === undefined || routingIdsEqual(row.ownerNodeRid, filter.ownerNodeRid))
    && (filter.ownerId === undefined || row.ownerId === filter.ownerId);
}
