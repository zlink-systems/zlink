import type { RoutingId } from '../Common';

export interface ZLinkSpotNodeBuilder {
  routingId(routingId: RoutingId): this;
  setRoutingIdPrefix(prefix: string): this;
  enableRouter(endpoint: string, routingId?: RoutingId, connect?: string | readonly string[]): this;
  connectRouter(endpoint: string): this;
  connectRouter(peerRid: RoutingId, endpoint: string): this;
  enablePubSub(endpoint: string, routingId?: RoutingId, connect?: string | readonly string[]): this;
  connectPeerPub(endpoint: string): this;
}
