package systems.zlink.stream.connector;

import java.net.URI;
import java.time.Duration;
import java.util.Locale;

final class ZLinkStreamConnectorConfiguration {
    private final ZLinkStreamConnectorOptions publicOptionsBase;
    private final URI endpoint;
    private final ZLinkStreamDispatchMode dispatchMode;
    private final Timeouts timeouts;
    private final Limits limits;
    private final Heartbeat heartbeat;
    private final Reconnect reconnect;
    private final Transport transport;

    private ZLinkStreamConnectorConfiguration(ZLinkStreamConnectorOptions options) {
        this.publicOptionsBase = options;
        this.endpoint = options.endpoint();
        this.dispatchMode = options.dispatchMode();
        this.timeouts =
                new Timeouts(
                        options.connectTimeout(), options.requestTimeout(), options.waitTimeout());
        this.limits = new Limits(options.maxSendPayloadSize(), options.maxReceivePayloadSize());
        this.heartbeat =
                new Heartbeat(
                        options.heartbeatEnabled(),
                        options.heartbeatInterval(),
                        options.heartbeatTimeout());
        this.reconnect =
                new Reconnect(
                        options.reconnectEnabled(),
                        options.maxReconnectAttempts(),
                        options.reconnectInitialDelay(),
                        options.reconnectMaxDelay(),
                        options.reconnectBackoffFactor());
        this.transport =
                new Transport(
                        transportFor(options.endpoint()),
                        options.skipServerCertificateValidation(),
                        options.compressionCodec());
    }

    /**
     * Validates every option and builds the runtime configuration.
     *
     * <p>Common connector spec §6.3: all of the options are checked before a connection is
     * attempted, a value outside its permitted range is {@code VALIDATION_FAILED}, and a
     * disagreement between two options is {@code CONFIGURATION_ERROR}. §9.2 requires the caller to
     * be able to read that code, so every rejection here is a {@link ZLinkStreamException}.
     */
    static ZLinkStreamConnectorConfiguration from(ZLinkStreamConnectorOptions options) {
        requireOption(options, "options");
        requireOption(options.endpoint(), "endpoint");
        //  Scheme/transport agreement is a cross-option check, so
        //  `transportFor` reports CONFIGURATION_ERROR (§3.1, §6.3).
        transportFor(options.endpoint());
        requireOption(options.dispatchMode(), "dispatchMode");
        requireOption(options.nameResolver(), "nameResolver");
        requirePositive(options.connectTimeout(), "connectTimeout");
        requirePositive(options.requestTimeout(), "requestTimeout");
        requirePositive(options.waitTimeout(), "waitTimeout");
        requirePositive(options.heartbeatInterval(), "heartbeatInterval");
        requirePositive(options.heartbeatTimeout(), "heartbeatTimeout");
        if (options.heartbeatEnabled()
                && !options.heartbeatTimeout().minus(options.heartbeatInterval()).isPositive()) {
            throw ZLinkStreamException.configurationError(
                    "heartbeatTimeout must be greater than heartbeatInterval");
        }
        requirePositive(options.reconnectInitialDelay(), "reconnectInitialDelay");
        requirePositive(options.reconnectMaxDelay(), "reconnectMaxDelay");
        if (options.reconnectBackoffFactor() < 1.0) {
            throw ZLinkStreamException.validationFailed(
                    "reconnectBackoffFactor must be at least 1.0");
        }
        if (options.maxReconnectAttempts()
                        < ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS
                || (options.reconnectEnabled() && options.maxReconnectAttempts() == 0)) {
            throw ZLinkStreamException.validationFailed(
                    "maxReconnectAttempts must be unlimited or positive");
        }
        if (options.maxSendPayloadSize() <= 0) {
            throw ZLinkStreamException.validationFailed("maxSendPayloadSize must be positive");
        }
        if (options.maxReceivePayloadSize() <= 0) {
            throw ZLinkStreamException.validationFailed("maxReceivePayloadSize must be positive");
        }
        requireOption(options.compression(), "compression");
        if (options.compression() == ZLinkStreamCompression.LZ4
                && options.compressionCodec() == null) {
            throw ZLinkStreamException.configurationError(
                    "compressionCodec is required when compression is lz4");
        }
        requireOption(options.typedCodec(), "typedCodec");
        return new ZLinkStreamConnectorConfiguration(options);
    }

    ZLinkStreamConnectorOptions publicOptions() {
        return publicOptionsBase;
    }

    URI endpoint() {
        return endpoint;
    }

    ZLinkStreamDispatchMode dispatchMode() {
        return dispatchMode;
    }

    Timeouts timeouts() {
        return timeouts;
    }

    Limits limits() {
        return limits;
    }

    Heartbeat heartbeat() {
        return heartbeat;
    }

    Reconnect reconnect() {
        return reconnect;
    }

    Transport transport() {
        return transport;
    }

    record Timeouts(Duration connect, Duration request, Duration waitForMessage) {}

    record Limits(int sendPayload, int receivePayload) {}

    record Heartbeat(boolean enabled, Duration interval, Duration timeout) {}

    record Reconnect(
            boolean enabled,
            int maxAttempts,
            Duration initialDelay,
            Duration maxDelay,
            double backoffFactor) {}

    record Transport(
            ZLinkStreamTransport kind,
            boolean skipServerCertificateValidation,
            ZLinkStreamCompressionCodec compressionCodec) {}

    private static <T> T requireOption(T value, String name) {
        if (value == null) {
            throw ZLinkStreamException.validationFailed(name + " is required");
        }
        return value;
    }

    private static void requirePositive(Duration value, String name) {
        requireOption(value, name);
        if (value.isZero() || value.isNegative()) {
            throw ZLinkStreamException.validationFailed(name + " must be positive");
        }
    }

    private static ZLinkStreamTransport transportFor(URI endpoint) {
        String scheme = endpoint.getScheme();
        if (scheme == null || scheme.isBlank()) {
            throw ZLinkStreamException.configurationError("endpoint URI scheme is required");
        }
        //  Endpoint notation policy §2.6: scheme is case-insensitive
        //  ("TCP://" must resolve the same as "tcp://").
        return switch (scheme.toLowerCase(Locale.ROOT)) {
            case "tcp" -> ZLinkStreamTransport.TCP;
            case "tls" -> ZLinkStreamTransport.TLS;
            case "ws" -> ZLinkStreamTransport.WEB_SOCKET;
            case "wss" -> ZLinkStreamTransport.WEB_SOCKET_SECURE;
            default ->
                    throw ZLinkStreamException.configurationError(
                            "unsupported endpoint URI scheme: " + scheme);
        };
    }
}
