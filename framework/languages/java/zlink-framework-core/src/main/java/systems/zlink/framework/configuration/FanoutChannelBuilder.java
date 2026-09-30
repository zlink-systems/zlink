package systems.zlink.framework.configuration;

import org.jspecify.annotations.Nullable;

import systems.zlink.contracts.core.RoutingId;

import java.time.Duration;

public interface FanoutChannelBuilder {
    FanoutChannelBuilder enablePublisher(String endpoint);

    FanoutChannelBuilder enablePublisher();

    FanoutChannelBuilder enablePublisher(int port);

    FanoutChannelBuilder setBindHost(String host);

    FanoutChannelBuilder setAdvertiseHost(String host);

    FanoutChannelBuilder setRoutingId(RoutingId routingId);

    FanoutChannelBuilder setRoutingIdPrefix(String prefix);

    FanoutChannelBuilder setNoDrop(boolean noDrop);

    /** Sets this publisher's send timeout, or restores the one-second default when null. */
    FanoutChannelBuilder setSendTimeout(@Nullable Duration value);

    FanoutChannelBuilder enableSubscriber();

    FanoutChannelBuilder subscribe(String topic);

    FanoutChannelBuilder connect(String endpoint);

    ZLinkEndpointConnections subscriberConnections();

    FanoutChannelBuilder addHandlerGroup(String groupName);

    void addPublishHandler(Class<?> handlerType, Class<?> messageType);

    void addPublishHandler(Class<?> handlerType, Class<?> messageType, String packetName);

    FanoutChannelBuilder addPublishHandler(Class<?> handlerType);

    FanoutChannelBuilder addPublishHandler(Class<?> handlerType, String packetName);
}
