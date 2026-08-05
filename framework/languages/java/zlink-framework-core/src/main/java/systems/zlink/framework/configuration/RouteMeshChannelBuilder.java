package systems.zlink.framework.runtime.internal.configuration;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.channels.ZLinkSocketRuntimeOptions;
import systems.zlink.framework.configuration.ZLinkEndpointConnections;

public interface RouteMeshChannelBuilder {
    RouteMeshChannelBuilder enableServer(String endpoint);

    ZLinkSocketRuntimeOptions configureServerSocket();

    RouteMeshChannelBuilder setRoutingId(RoutingId routingId);

    RouteMeshChannelBuilder setDefaultRequestTimeout(java.time.Duration timeout);

    RouteMeshChannelBuilder enableClient();

    RouteMeshChannelBuilder enableClient(String endpoint);

    ZLinkEndpointConnections clientConnections();

    RouteMeshChannelBuilder addHandlerGroup(String groupName);

    void addSendHandler(
        Class<?> handlerType,
        Class<?> messageType);

    void addSendHandler(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName);

    void addRequestHandler(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType);

    void addRequestHandler(
        Class<?> handlerType,
        Class<?> requestType,
        Class<?> replyType,
        String packetName);
}
