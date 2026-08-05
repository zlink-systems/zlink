package systems.zlink.framework.configuration;

import systems.zlink.contracts.core.RoutingId;

public interface FanoutChannelBuilder {
    FanoutChannelBuilder enablePublisher(String endpoint);

    FanoutChannelBuilder enablePublisher();

    FanoutChannelBuilder enablePublisher(int port);

    FanoutChannelBuilder setBindHost(String host);

    FanoutChannelBuilder setAdvertiseHost(String host);

    FanoutChannelBuilder setRoutingId(RoutingId routingId);

    FanoutChannelBuilder setRoutingIdPrefix(String prefix);

    FanoutChannelBuilder enableSubscriber();

    FanoutChannelBuilder connect(String endpoint);

    ZLinkEndpointConnections subscriberConnections();

    FanoutChannelBuilder addHandlerGroup(String groupName);

    void addPublishHandler(
        Class<?> handlerType,
        Class<?> messageType);

    void addPublishHandler(
        Class<?> handlerType,
        Class<?> messageType,
        String packetName);

    FanoutChannelBuilder addPublishHandler(Class<?> handlerType);

    FanoutChannelBuilder addPublishHandler(Class<?> handlerType, String packetName);
}
