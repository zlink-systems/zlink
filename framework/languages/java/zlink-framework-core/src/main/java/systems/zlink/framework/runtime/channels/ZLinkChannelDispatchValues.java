package systems.zlink.framework.runtime.channels;

import java.util.Map;
import java.util.Optional;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkHandlerDispatchKind;
import systems.zlink.framework.ZLinkHandlerFilterContext;
import systems.zlink.framework.channels.ZLinkPublishMessageContext;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;

record ParsedPacket(String packetName, Message payload) {
}

final class DefaultHandlerFilterContext
    implements ZLinkHandlerFilterContext {
    private final ZLinkMessageContext message;
    private final ZLinkHandlerDispatchKind dispatchKind;

    DefaultHandlerFilterContext(
        ZLinkMessageContext message,
        ZLinkHandlerDispatchKind dispatchKind) {
        this.message = java.util.Objects.requireNonNull(message, "message");
        this.dispatchKind =
            java.util.Objects.requireNonNull(dispatchKind, "dispatchKind");
    }

    @Override
    public Optional<String> meshName() {
        return message.meshName();
    }

    @Override
    public Optional<String> channelName() {
        return message.channelName();
    }

    @Override
    public String packetName() {
        return message.packetName();
    }

    @Override
    public Optional<String> contentType() {
        return message.contentType();
    }

    @Override
    public Map<String, String> metadata() {
        return message.metadata();
    }

    @Override
    public Optional<String> correlationId() {
        return message.correlationId();
    }

    @Override
    public ZLinkHandlerDispatchKind dispatchKind() {
        return dispatchKind;
    }
}

final class PayloadDecodeDispatchException extends RuntimeException {
    PayloadDecodeDispatchException(String message, Throwable cause) {
        super(message, cause);
    }
}

abstract class ChannelHandlerContextBase implements ZLinkMessageContext {
    private final String meshName;
    private final String channelName;
    private final String packetName;
    private final String contentType;
    private final Map<String, String> metadata;

    ChannelHandlerContextBase(
        String channelName,
        String packetName,
        String contentType) {
        this(null, channelName, packetName, contentType, Map.of());
    }

    ChannelHandlerContextBase(
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        this(null, channelName, packetName, contentType, metadata);
    }

    ChannelHandlerContextBase(
        String meshName,
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        this.meshName = meshName;
        this.channelName = channelName;
        this.packetName = packetName;
        this.contentType = contentType;
        this.metadata = Map.copyOf(metadata);
    }

    @Override
    public final Optional<String> meshName() {
        return presentText(meshName);
    }

    @Override
    public final Optional<String> channelName() {
        return presentText(channelName);
    }

    @Override
    public final String packetName() {
        return packetName;
    }

    @Override
    public final Optional<String> contentType() {
        return presentText(contentType);
    }

    @Override
    public final Map<String, String> metadata() {
        return metadata;
    }

    @Override
    public final Optional<String> correlationId() {
        return Optional.empty();
    }

    private static Optional<String> presentText(String value) {
        return Optional.ofNullable(value).filter(text -> !text.isBlank());
    }
}

final class DefaultRequestContext
    extends ChannelHandlerContextBase
    implements ZLinkMessageContext {
    DefaultRequestContext(String channelName, String packetName, String contentType) {
        super(channelName, packetName, contentType);
    }

    DefaultRequestContext(
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, contentType, metadata);
    }

    DefaultRequestContext(
        String meshName,
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        super(meshName, channelName, packetName, contentType, metadata);
    }
}

final class DefaultSendContext
    extends ChannelHandlerContextBase
    implements ZLinkMessageContext {
    DefaultSendContext(String channelName, String packetName, String contentType) {
        super(channelName, packetName, contentType);
    }

    DefaultSendContext(
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, contentType, metadata);
    }

    DefaultSendContext(
        String meshName,
        String channelName,
        String packetName,
        String contentType,
        Map<String, String> metadata) {
        super(meshName, channelName, packetName, contentType, metadata);
    }
}

final class DefaultPublishContext
    extends ChannelHandlerContextBase
    implements ZLinkPublishMessageContext {
    private final String topic;

    DefaultPublishContext(
        String channelName,
        String packetName,
        String topic,
        String contentType) {
        super(channelName, packetName, contentType);
        this.topic = topic;
    }

    DefaultPublishContext(
        String channelName,
        String packetName,
        String topic,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, contentType, metadata);
        this.topic = topic;
    }

    @Override
    public String topic() {
        return topic;
    }

    @Override
    public Optional<String> source() {
        return Optional.empty();
    }
}

abstract class RouteHandlerContextBase extends ChannelHandlerContextBase {
    private final RoutingId routingId;

    RouteHandlerContextBase(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType) {
        super(channelName, packetName, contentType);
        this.routingId = routingId;
    }

    RouteHandlerContextBase(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, contentType, metadata);
        this.routingId = routingId;
    }

    RouteHandlerContextBase(
        String meshName,
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(meshName, channelName, packetName, contentType, metadata);
        this.routingId = routingId;
    }

    public final RoutingId routingId() {
        return routingId;
    }

    public final RoutingId sourceNodeRid() {
        return routingId;
    }
}

final class DefaultRouteRequestContext
    extends RouteHandlerContextBase
    implements ZLinkRouteMessageContext {
    DefaultRouteRequestContext(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType) {
        super(channelName, packetName, routingId, contentType);
    }

    DefaultRouteRequestContext(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, routingId, contentType, metadata);
    }

    DefaultRouteRequestContext(
        String meshName,
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(meshName, channelName, packetName, routingId, contentType, metadata);
    }
}

final class DefaultRouteSendContext
    extends RouteHandlerContextBase
    implements ZLinkRouteMessageContext {
    DefaultRouteSendContext(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType) {
        super(channelName, packetName, routingId, contentType);
    }

    DefaultRouteSendContext(
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(channelName, packetName, routingId, contentType, metadata);
    }

    DefaultRouteSendContext(
        String meshName,
        String channelName,
        String packetName,
        RoutingId routingId,
        String contentType,
        Map<String, String> metadata) {
        super(meshName, channelName, packetName, routingId, contentType, metadata);
    }
}

final class NoDataBackoff {
    private static final int MAX_MISSES = 6;
    private static final long MAX_DELAY_MILLIS = 20L;
    private int misses;

    void reset() {
        misses = 0;
    }

    long nextDelayNanos() {
        misses = Math.min(misses + 1, MAX_MISSES);
        long delayMillis = Math.min(1L << (misses - 1), MAX_DELAY_MILLIS);
        return TimeUnit.MILLISECONDS.toNanos(delayMillis);
    }
}
