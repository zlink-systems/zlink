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

    public static ZLinkStreamConnectorOptions createDefault(URI endpoint) {
        return new ZLinkStreamConnectorOptions(
                endpoint,
                ZLinkStreamDispatchMode.MANUAL,
                Duration.ofSeconds(30),
                Duration.ofSeconds(5),
                3,
                Duration.ofSeconds(5),
                64 * 1024,
                64 * 1024,
                true,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                true,
                Duration.ofMillis(250),
                Duration.ofSeconds(5),
                2.0,
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
                Duration.ofSeconds(5),
                maxReconnectAttempts,
                Duration.ofSeconds(5),
                64 * 1024,
                64 * 1024,
                true,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                true,
                Duration.ofMillis(250),
                Duration.ofSeconds(5),
                2.0,
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
                Duration.ofSeconds(5),
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                64 * 1024,
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
                Duration.ofSeconds(5),
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                64 * 1024,
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
                Duration.ofSeconds(5),
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                64 * 1024,
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
                Duration.ofSeconds(5),
                maxReconnectAttempts,
                connectTimeout,
                maxSendPayloadSize,
                64 * 1024,
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
