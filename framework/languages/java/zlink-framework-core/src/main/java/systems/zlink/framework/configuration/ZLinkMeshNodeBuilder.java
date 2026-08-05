package systems.zlink.framework.configuration;

import java.time.Duration;

public interface ZLinkMeshNodeBuilder {
    ZLinkMeshChannelBuilder channelName(String channelName);

    ZLinkMeshNodeBuilder listen(String endpoint);

    ZLinkMeshNodeBuilder listen();

    ZLinkMeshNodeBuilder listen(int port);

    ZLinkMeshNodeBuilder setBindHost(String host);

    ZLinkMeshNodeBuilder setAdvertiseHost(String host);

    ZLinkMeshNodeBuilder setRoutingId(
        systems.zlink.contracts.core.RoutingId routingId);

    ZLinkMeshNodeBuilder setRoutingIdPrefix(String prefix);

    ZLinkMeshNodeBuilder setPlacementWeight(int weight);

    ZLinkMeshNodeBuilder setActorCapacity(int maxActors);

    ZLinkMeshNodeBuilder setSpotCapacity(int maxSpots);

    ZLinkMeshNodeBuilder setActivationConcurrency(
        int maxConcurrentActivations);

    ZLinkMeshNodeBuilder setInstanceSpotIdleTimeout(Duration timeout);

    ZLinkMeshNodeSocketConfig configureRouterSocket();

    ZLinkSpotPublisherConfig configureSpotPublisher();

    ZLinkMeshPeerConnections peerConnections();

    ZLinkMeshNodeBuilder setDefaultRequestTimeout(Duration timeout);

    ZLinkMeshObjectRoleBuilder objects();

    <THandler, TMessage>
    ZLinkMeshNodeBuilder addRouteSendHandler(
        Class<THandler> handlerType,
        Class<TMessage> messageType);

    <THandler, TRequest, TReply>
    ZLinkMeshNodeBuilder addRouteRequestHandler(
        Class<THandler> handlerType,
        Class<TRequest> requestType,
        Class<TReply> replyType);

}
