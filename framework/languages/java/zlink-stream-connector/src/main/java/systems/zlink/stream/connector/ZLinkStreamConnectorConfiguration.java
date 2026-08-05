package systems.zlink.stream.connector;

import java.net.URI;
import java.time.Duration;
import java.util.Objects;

final class ZLinkStreamConnectorConfiguration {
    private final ZLinkStreamConnectorOptions publicOptions;
    private final URI endpoint;
    private final ZLinkStreamDispatchMode dispatchMode;
    private final Timeouts timeouts;
    private final Limits limits;
    private final Heartbeat heartbeat;
    private final Reconnect reconnect;
    private final Transport transport;

    private ZLinkStreamConnectorConfiguration(ZLinkStreamConnectorOptions options) {
        this.publicOptions = options;
        this.endpoint = options.endpoint();
        this.dispatchMode = options.dispatchMode();
        this.timeouts = new Timeouts(
            options.connectTimeout(), options.requestTimeout(), options.waitTimeout());
        this.limits = new Limits(
            options.maxSendPayloadSize(),
            options.maxReceivePayloadSize(),
            options.maxReceivedMessages(),
            options.maxInboundObserverNotifications(),
            options.maxInboundObserverPayloadPreviewBytes());
        this.heartbeat = new Heartbeat(
            options.heartbeatEnabled(), options.heartbeatInterval(), options.heartbeatTimeout());
        this.reconnect = new Reconnect(
            options.reconnectEnabled(),
            options.maxReconnectAttempts(),
            options.reconnectInitialDelay(),
            options.reconnectMaxDelay(),
            options.reconnectBackoffFactor());
        this.transport = new Transport(
            transportFor(options.endpoint()),
            options.skipServerCertificateValidation(), options.compressionCodec());
    }

    static ZLinkStreamConnectorConfiguration from(ZLinkStreamConnectorOptions options) {
        Objects.requireNonNull(options, "options");
        Objects.requireNonNull(options.endpoint(), "endpoint");
        transportFor(options.endpoint());
        Objects.requireNonNull(options.dispatchMode(), "dispatchMode");
        Objects.requireNonNull(options.nameResolver(), "nameResolver");
        requirePositive(options.connectTimeout(), "connectTimeout");
        requirePositive(options.requestTimeout(), "requestTimeout");
        requirePositive(options.waitTimeout(), "waitTimeout");
        requirePositive(options.heartbeatInterval(), "heartbeatInterval");
        requirePositive(options.heartbeatTimeout(), "heartbeatTimeout");
        if (options.heartbeatEnabled()
            && !options.heartbeatTimeout().minus(options.heartbeatInterval()).isPositive()) {
            throw new IllegalArgumentException(
                "heartbeatTimeout must be greater than heartbeatInterval");
        }
        requirePositive(options.reconnectInitialDelay(), "reconnectInitialDelay");
        requirePositive(options.reconnectMaxDelay(), "reconnectMaxDelay");
        if (options.reconnectBackoffFactor() < 1.0) {
            throw new IllegalArgumentException("reconnectBackoffFactor must be at least 1.0");
        }
        if (options.maxReconnectAttempts() < ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS
            || (options.reconnectEnabled() && options.maxReconnectAttempts() == 0)) {
            throw new IllegalArgumentException("maxReconnectAttempts must be unlimited or positive");
        }
        if (options.maxSendPayloadSize() <= 0) {
            throw new IllegalArgumentException("maxSendPayloadSize must be positive");
        }
        if (options.maxReceivePayloadSize() <= 0) {
            throw new IllegalArgumentException("maxReceivePayloadSize must be positive");
        }
        if (options.maxReceivedMessages() <= 0) {
            throw new IllegalArgumentException("maxReceivedMessages must be positive");
        }
        if (options.maxInboundObserverNotifications() <= 0) {
            throw new IllegalArgumentException(
                "maxInboundObserverNotifications must be positive");
        }
        if (options.maxInboundObserverPayloadPreviewBytes() < 0) {
            throw new IllegalArgumentException(
                "maxInboundObserverPayloadPreviewBytes must not be negative");
        }
        Objects.requireNonNull(options.compression(), "compression");
        return new ZLinkStreamConnectorConfiguration(options);
    }

    ZLinkStreamConnectorOptions publicOptions() { return publicOptions; }
    URI endpoint() { return endpoint; }
    ZLinkStreamDispatchMode dispatchMode() { return dispatchMode; }
    Timeouts timeouts() { return timeouts; }
    Limits limits() { return limits; }
    Heartbeat heartbeat() { return heartbeat; }
    Reconnect reconnect() { return reconnect; }
    Transport transport() { return transport; }

    record Timeouts(Duration connect, Duration request, Duration waitForMessage) { }
    record Limits(
        int sendPayload,
        int receivePayload,
        int receivedMessages,
        int inboundObserverNotifications,
        int inboundObserverPayloadPreviewBytes) { }
    record Heartbeat(boolean enabled, Duration interval, Duration timeout) { }
    record Reconnect(
        boolean enabled,
        int maxAttempts,
        Duration initialDelay,
        Duration maxDelay,
        double backoffFactor) { }
    record Transport(
        ZLinkStreamTransport kind,
        boolean skipServerCertificateValidation,
        ZLinkStreamCompressionCodec compressionCodec) { }

    private static void requirePositive(Duration value, String name) {
        Objects.requireNonNull(value, name);
        if (value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(name + " must be positive");
        }
    }

    private static ZLinkStreamTransport transportFor(URI endpoint) {
        String scheme = endpoint.getScheme();
        if (scheme == null || scheme.isBlank()) {
            throw new IllegalArgumentException("endpoint URI scheme is required");
        }
        return switch (scheme) {
            case "tcp" -> ZLinkStreamTransport.TCP;
            case "tls" -> ZLinkStreamTransport.TLS;
            case "ws" -> ZLinkStreamTransport.WEB_SOCKET;
            case "wss" -> ZLinkStreamTransport.WEB_SOCKET_SECURE;
            default -> throw new IllegalArgumentException(
                "unsupported endpoint URI scheme: " + scheme);
        };
    }
}
