import type { RoutingId } from '../../contracts';
import type { ZLinkBackendSpot, ZLinkBackendSpotNode } from '../backend/contracts';
import type { ZLinkFrameworkRegistration } from '../configuration';
import type { ZLinkSpotRouteTarget } from '../spots/spot-routing-internal';

export class ZLinkSpotRouteTargetResolver {
  private spotNodes?: ReadonlyMap<string, ZLinkBackendSpotNode>;

  constructor(private readonly registration: ZLinkFrameworkRegistration) {}

  setSpotNodes(spotNodes: ReadonlyMap<string, ZLinkBackendSpotNode>): void {
    this.spotNodes = spotNodes;
  }

  routeNode(routerChannelId: string): ZLinkBackendSpotNode | undefined {
    const named = this.registration.spotNodes.get(routerChannelId);
    if (named?.router !== undefined) {
      return this.spotNodes?.get(routerChannelId);
    }
    const channelOwners = [...this.registration.spotNodes.entries()]
      .filter(([, node]) => Object.prototype.hasOwnProperty.call(node.meshChannels ?? {}, routerChannelId));
    if (channelOwners.length === 1) {
      return this.spotNodes?.get(channelOwners[0][0]);
    }
    const routeChannel = this.registration.routeChannelOptions.get(routerChannelId);
    if (routeChannel === undefined) {
      return undefined;
    }
    if (routeChannel.routingId !== undefined) {
      for (const [spotNodeName, spotNode] of this.registration.spotNodes.entries()) {
        if (spotNode.router?.routingId === routeChannel.routingId) {
          return this.spotNodes?.get(spotNodeName);
        }
      }
    }
    const routerNodeNames = [...this.registration.spotNodes.entries()]
      .filter(([, spotNode]) => spotNode.router !== undefined)
      .map(([spotNodeName]) => spotNodeName);
    return routerNodeNames.length === 1 ? this.spotNodes?.get(routerNodeNames[0]) : undefined;
  }

  localRouteNode(target: ZLinkSpotRouteTarget): ZLinkBackendSpotNode | undefined {
    const routeNode = this.routeNode(target.routerChannelId) ?? this.spotNodes?.get(target.routerChannelId);
    return routeNode !== undefined && routingIdsMatchSpotRoute(routeNode.routingId, target.targetNodeRid)
      ? routeNode
      : undefined;
  }

  spotNodeRouter(routerChannelId: string): ZLinkBackendSpot | undefined {
    if (this.registration.spotNodes.get(routerChannelId)?.router !== undefined) {
      return this.spotNodes?.get(routerChannelId)?.entrySpot();
    }
    const routeBridgeNode = this.routeNode(routerChannelId);
    return (routeBridgeNode ?? this.spotNodes?.get(routerChannelId))?.entrySpot();
  }

  canRouteChannel(routerChannelId: string): boolean {
    return this.registration.routeChannels.has(routerChannelId)
      || this.spotNodeRouter(routerChannelId) !== undefined;
  }

  canRoutePacketChannel(routerChannelId: string): boolean {
    if (this.spotNodes?.has(routerChannelId) ?? false) {
      return false;
    }
    return this.registration.routeChannels.has(routerChannelId);
  }

  hasBoundRouteRouter(routerChannelId: string): boolean {
    return this.registration.routeChannelOptions.get(routerChannelId)?.bind !== undefined;
  }

  hasNamedSpotNode(routerChannelId: string): boolean {
    return this.registration.spotNodes.has(routerChannelId);
  }

}

function routingIdsMatchSpotRoute(left: RoutingId | undefined, right: RoutingId | undefined): boolean {
  if (left === undefined || right === undefined) {
    return false;
  }
  const rightCandidates = spotRouteRoutingIdCandidates(right);
  return [...spotRouteRoutingIdCandidates(left)].some(candidate => rightCandidates.has(candidate));
}

function spotRouteRoutingIdCandidates(routingId: RoutingId): ReadonlySet<string> {
  const candidates = new Set<string>();
  const toHex = (routingId as unknown as { toHex?: () => string }).toHex;
  if (typeof toHex === 'function') {
    candidates.add(toHex.call(routingId).toLowerCase());
  }
  const text = String(routingId);
  candidates.add(text.toLowerCase());
  try {
    candidates.add(Buffer.from(text, 'utf8').toString('hex'));
  } catch {
    // This legacy route path also receives routing ids already encoded as hex strings.
  }
  return candidates;
}
