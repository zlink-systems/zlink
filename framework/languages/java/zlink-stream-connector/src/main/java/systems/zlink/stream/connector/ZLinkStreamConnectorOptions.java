package systems.zlink.stream.connector;

import java.net.URI;
import java.time.Duration;

public record ZLinkStreamConnectorOptions(
        URI endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        Duration requestTimeout,
        Duration waitTimeout,
        int maxReconnectAttempts,
        Duration connectTimeout,
        int maxSendPayloadSize,
        int maxReceivePayloadSize,
        boolean heartbeatEnabled,
        Duration heartbeatInterval,
        Duration heartbeatTimeout,
        boolean reconnectEnabled,
        Duration reconnectInitialDelay,
        Duration reconnectMaxDelay,
        double reconnectBackoffFactor,
        boolean skipServerCertificateValidation,
        ZLinkStreamCompression compression,
        ZLinkStreamCompressionCodec compressionCodec,
        ZLinkStreamPacketNameResolver nameResolver,
        ZLinkStreamTypedCodec typedCodec) {
    public static final int UNLIMITED_RECONNECT_ATTEMPTS = -1;
    static final int DEFAULT_MAX_PAYLOAD_SIZE = 64 * 1024;
    private static final Duration DEFAULT_REQUEST_TIMEOUT = Duration.ofSeconds(30);
    private static final Duration DEFAULT_WAIT_TIMEOUT = Duration.ofSeconds(5);
    private static final int DEFAULT_MAX_RECONNECT_ATTEMPTS = 3;
    private static final Duration DEFAULT_CONNECT_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration DEFAULT_HEARTBEAT_INTERVAL = Duration.ofSeconds(1);
    private static final Duration DEFAULT_HEARTBEAT_TIMEOUT = Duration.ofSeconds(5);
    private static final Duration DEFAULT_RECONNECT_INITIAL_DELAY = Duration.ofMillis(250);
    private static final Duration DEFAULT_RECONNECT_MAX_DELAY = Duration.ofSeconds(5);
    private static final double DEFAULT_RECONNECT_BACKOFF_FACTOR = 2.0;

    public static ZLinkStreamConnectorOptions createDefault(URI endpoint) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                ZLinkStreamDispatchMode.MANUAL,
                DEFAULT_REQUEST_TIMEOUT,
                DEFAULT_WAIT_TIMEOUT,
                DEFAULT_MAX_RECONNECT_ATTEMPTS,
                DEFAULT_CONNECT_TIMEOUT,
                DEFAULT_MAX_PAYLOAD_SIZE,
                DEFAULT_MAX_PAYLOAD_SIZE,
                true,
                DEFAULT_HEARTBEAT_INTERVAL,
                DEFAULT_HEARTBEAT_TIMEOUT,
                true,
                DEFAULT_RECONNECT_INITIAL_DELAY,
                DEFAULT_RECONNECT_MAX_DELAY,
                DEFAULT_RECONNECT_BACKOFF_FACTOR,
                false,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    public ZLinkStreamConnectorOptions {
        if (compression == null) {
            compression = ZLinkStreamCompression.LZ4;
        }
        if (compression == ZLinkStreamCompression.NONE && compressionCodec != null) {
            //  Common connector spec §6.3 names this as a disagreement
            //  between two options, so the code is CONFIGURATION_ERROR and
            //  §9.2 requires it to travel in an exception that carries it.
            throw ZLinkStreamException.configurationError(
                    "compressionCodec cannot be set when compression is none");
        }
        if (compression == ZLinkStreamCompression.LZ4 && compressionCodec == null) {
            compressionCodec = ZLinkStreamCompressionCodecs.lz4();
        }
        if (nameResolver == null) {
            nameResolver = ZLinkStreamPacketNameResolver.defaultResolver();
        }
        if (typedCodec == null) {
            typedCodec = ZLinkStreamJson.codec();
        }
    }

    public ZLinkStreamConnectorOptions(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            Duration requestTimeout,
            int maxReconnectAttempts) {
        this(
                endpoint,
                dispatchMode,
                requestTimeout,
                DEFAULT_WAIT_TIMEOUT,
                maxReconnectAttempts,
                DEFAULT_CONNECT_TIMEOUT,
                DEFAULT_MAX_PAYLOAD_SIZE,
                DEFAULT_MAX_PAYLOAD_SIZE,
                true,
                DEFAULT_HEARTBEAT_INTERVAL,
                DEFAULT_HEARTBEAT_TIMEOUT,
                true,
                DEFAULT_RECONNECT_INITIAL_DELAY,
                DEFAULT_RECONNECT_MAX_DELAY,
                DEFAULT_RECONNECT_BACKOFF_FACTOR,
                false,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    public ZLinkStreamConnectorOptions(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            Duration requestTimeout,
            int maxReconnectAttempts,
            Duration connectTimeout,
            int maxSendPayloadSize,
            boolean heartbeatEnabled,
            Duration heartbeatInterval,
            Duration heartbeatTimeout,
            boolean reconnectEnabled,
            Duration reconnectInitialDelay,
            Duration reconnectMaxDelay,
            double reconnectBackoffFactor) {
        this(
                endpoint,
                dispatchMode,
                requestTimeout,
                DEFAULT_WAIT_TIMEOUT,
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                DEFAULT_MAX_PAYLOAD_SIZE,
                heartbeatEnabled,
                heartbeatInterval,
                heartbeatTimeout,
                reconnectEnabled,
                reconnectInitialDelay,
                reconnectMaxDelay,
                reconnectBackoffFactor,
                false,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    public ZLinkStreamConnectorOptions(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            Duration requestTimeout,
            int maxReconnectAttempts,
            Duration connectTimeout,
            int maxSendPayloadSize,
            boolean heartbeatEnabled,
            Duration heartbeatInterval,
            Duration heartbeatTimeout,
            boolean reconnectEnabled,
            Duration reconnectInitialDelay,
            Duration reconnectMaxDelay,
            double reconnectBackoffFactor,
            boolean skipServerCertificateValidation) {
        this(
                endpoint,
                dispatchMode,
                requestTimeout,
                DEFAULT_WAIT_TIMEOUT,
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                DEFAULT_MAX_PAYLOAD_SIZE,
                heartbeatEnabled,
                heartbeatInterval,
                heartbeatTimeout,
                reconnectEnabled,
                reconnectInitialDelay,
                reconnectMaxDelay,
                reconnectBackoffFactor,
                skipServerCertificateValidation,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                null);
    }

    public ZLinkStreamConnectorOptions(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            Duration requestTimeout,
            int maxReconnectAttempts,
            Duration connectTimeout,
            int maxSendPayloadSize,
            boolean heartbeatEnabled,
            Duration heartbeatInterval,
            Duration heartbeatTimeout,
            boolean reconnectEnabled,
            Duration reconnectInitialDelay,
            Duration reconnectMaxDelay,
            double reconnectBackoffFactor,
            ZLinkStreamTypedCodec typedCodec) {
        this(
                endpoint,
                dispatchMode,
                requestTimeout,
                DEFAULT_WAIT_TIMEOUT,
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                DEFAULT_MAX_PAYLOAD_SIZE,
                heartbeatEnabled,
                heartbeatInterval,
                heartbeatTimeout,
                reconnectEnabled,
                reconnectInitialDelay,
                reconnectMaxDelay,
                reconnectBackoffFactor,
                false,
                ZLinkStreamCompression.LZ4,
                null,
                ZLinkStreamPacketNameResolver.defaultResolver(),
                typedCodec);
    }

    public ZLinkStreamConnectorOptions(
            URI endpoint,
            ZLinkStreamDispatchMode dispatchMode,
            Duration requestTimeout,
            int maxReconnectAttempts,
            Duration connectTimeout,
            int maxSendPayloadSize,
            boolean heartbeatEnabled,
            Duration heartbeatInterval,
            Duration heartbeatTimeout,
            boolean reconnectEnabled,
            Duration reconnectInitialDelay,
            Duration reconnectMaxDelay,
            double reconnectBackoffFactor,
            boolean skipServerCertificateValidation,
            ZLinkStreamPacketNameResolver nameResolver) {
        this(
                endpoint,
                dispatchMode,
                requestTimeout,
                DEFAULT_WAIT_TIMEOUT,
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                DEFAULT_MAX_PAYLOAD_SIZE,
                heartbeatEnabled,
                heartbeatInterval,
                heartbeatTimeout,
                reconnectEnabled,
                reconnectInitialDelay,
                reconnectMaxDelay,
                reconnectBackoffFactor,
                skipServerCertificateValidation,
                ZLinkStreamCompression.LZ4,
                null,
                nameResolver,
                null);
    }
}
