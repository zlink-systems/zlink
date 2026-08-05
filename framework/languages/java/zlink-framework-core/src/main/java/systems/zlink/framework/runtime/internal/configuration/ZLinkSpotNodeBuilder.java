package systems.zlink.framework.runtime.internal.configuration;

import systems.zlink.contracts.core.RoutingId;

public interface ZLinkSpotNodeBuilder {
    ZLinkSpotNodeBuilder setRoutingId(RoutingId routingId);

    ZLinkSpotNodeBuilder enableRouter(String endpoint);

    ZLinkSpotNodeBuilder connectRouter(String endpoint);

    ZLinkSpotNodeBuilder connectRouter(RoutingId peerRoutingId, String endpoint);

    ZLinkSpotNodeBuilder enablePubSub(String endpoint);

    ZLinkSpotNodeBuilder connectPeerPub(String endpoint);

}
