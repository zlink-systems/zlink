package systems.zlink.stream.connector;

import java.net.URI;
import java.time.Duration;
import java.util.Locale;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkStreamConnectorConfiguration {
    //  The construction-time options record, held with its diagnostics level
    //  frozen at whatever value the caller passed at creation. `publicOptions()`
    //  layers the *current* value of `diagnosticsLevelCell` on top of this
    //  base so that `options()` always agrees with the runtime-mutable level
    //  (server spec 26 §4.1 / common connector spec §13): the cell is the
    //  single source of truth, never the cached record.
    private final ZLinkStreamConnectorOptions publicOptionsBase;
    private final URI endpoint;
    private final ZLinkStreamDispatchMode dispatchMode;
    private final Timeouts timeouts;
    private final Limits limits;
    private final Heartbeat heartbeat;
    private final Reconnect reconnect;
    private final Transport transport;
    //  Runtime-mutable diagnostics level cell. Application code may read/change
    //  it without recreating the connector (server spec 26 §4.1); each
    //  processing point reads it exactly once via `diagnosticsLevel()` or the
    //  static `flowCaptureEnabled(level)` gate below and threads that single
    //  value through its own processing instead of re-reading the cell.
    private final AtomicReference<ZLinkStreamDiagnosticsLevel> diagnosticsLevelCell;

    private ZLinkStreamConnectorConfiguration(ZLinkStreamConnectorOptions options) {
        this.publicOptionsBase = options;
        this.endpoint = options.endpoint();
        this.dispatchMode = options.dispatchMode();
        this.diagnosticsLevelCell = new AtomicReference<>(options.diagnosticsLevel());
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
        Objects.requireNonNull(options.diagnosticsLevel(), "diagnosticsLevel");
        return new ZLinkStreamConnectorConfiguration(options);
    }

    /**
     * Returns the options record with {@code diagnosticsLevel} refreshed to
     * the current value of the runtime cell, so a caller reading
     * {@code options().diagnosticsLevel()} always observes the level that is
     * actually in effect (never the value frozen at construction time).
     */
    ZLinkStreamConnectorOptions publicOptions() {
        return publicOptionsBase.withDiagnosticsLevel(diagnosticsLevelCell.get());
    }

    URI endpoint() { return endpoint; }
    ZLinkStreamDispatchMode dispatchMode() { return dispatchMode; }
    Timeouts timeouts() { return timeouts; }
    Limits limits() { return limits; }
    Heartbeat heartbeat() { return heartbeat; }
    Reconnect reconnect() { return reconnect; }
    Transport transport() { return transport; }

    /**
     * Single atomic read of the current diagnostics level. Callers must read
     * this exactly once per processing point (one outbound submit, one
     * inbound frame dispatch) and thread the returned value through that
     * processing instead of reading the cell again, so a level flip that
     * lands mid-processing never produces an internally inconsistent
     * decision (server spec 26 §4.1 / common connector spec §13).
     */
    ZLinkStreamDiagnosticsLevel diagnosticsLevel() { return diagnosticsLevelCell.get(); }

    /**
     * Atomically installs a new diagnostics level. The change applies to
     * processing points that read the level after this call returns;
     * frames already built under the previous level are never retroactively
     * changed.
     */
    void diagnosticsLevel(ZLinkStreamDiagnosticsLevel level) {
        diagnosticsLevelCell.set(Objects.requireNonNull(level, "diagnosticsLevel"));
    }

    /** Spec 27 §4 gate: trace-only flow work is skipped entirely at Off. */
    static boolean flowCaptureEnabled(ZLinkStreamDiagnosticsLevel level) {
        return level != ZLinkStreamDiagnosticsLevel.OFF;
    }

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
        //  Endpoint notation policy §2.6: scheme is case-insensitive
        //  ("TCP://" must resolve the same as "tcp://").
        return switch (scheme.toLowerCase(Locale.ROOT)) {
            case "tcp" -> ZLinkStreamTransport.TCP;
            case "tls" -> ZLinkStreamTransport.TLS;
            case "ws" -> ZLinkStreamTransport.WEB_SOCKET;
            case "wss" -> ZLinkStreamTransport.WEB_SOCKET_SECURE;
            default -> throw new IllegalArgumentException(
                "unsupported endpoint URI scheme: " + scheme);
        };
    }
}
