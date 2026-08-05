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
    int maxReceivedMessages,
    int maxInboundObserverNotifications,
    int maxInboundObserverPayloadPreviewBytes,
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
            3,
            Duration.ofSeconds(5),
            64 * 1024,
            64 * 1024,
            1024,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false,
            ZLinkStreamCompression.LZ4,
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null);
    }

    public ZLinkStreamConnectorOptions {
        if (compression == null) {
            compression = ZLinkStreamCompression.LZ4;
        }
        if (compression == ZLinkStreamCompression.NONE && compressionCodec != null) {
            throw new IllegalArgumentException("compressionCodec cannot be set when compression is none");
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
        Duration waitTimeout,
        int maxReconnectAttempts,
        Duration connectTimeout,
        int maxSendPayloadSize,
        int maxReceivePayloadSize,
        int maxReceivedMessages,
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
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            waitTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            maxReceivePayloadSize,
            maxReceivedMessages,
            1024,
            0,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
            compressionCodec,
            nameResolver,
            typedCodec);
    }

    public ZLinkStreamConnectorOptions(
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
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            waitTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            maxReceivePayloadSize,
            Integer.MAX_VALUE,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
            compressionCodec,
            nameResolver,
            typedCodec);
    }

    public ZLinkStreamConnectorOptions(
        URI endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        Duration requestTimeout,
        int maxReconnectAttempts,
        Duration connectTimeout,
        int maxSendPayloadSize,
        int maxReceivePayloadSize,
        int maxReceivedMessages,
        boolean heartbeatEnabled,
        Duration heartbeatInterval,
        Duration heartbeatTimeout,
        boolean reconnectEnabled,
        Duration reconnectInitialDelay,
        Duration reconnectMaxDelay,
        double reconnectBackoffFactor,
        boolean skipServerCertificateValidation,
        ZLinkStreamCompression compression,
        ZLinkStreamPacketNameResolver nameResolver,
        ZLinkStreamTypedCodec typedCodec) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            Duration.ofSeconds(5),
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            maxReceivePayloadSize,
            maxReceivedMessages,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
            null,
            nameResolver,
            typedCodec);
    }

    public ZLinkStreamConnectorOptions(
        URI endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        Duration requestTimeout,
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
        ZLinkStreamPacketNameResolver nameResolver,
        ZLinkStreamTypedCodec typedCodec) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            maxReceivePayloadSize,
            Integer.MAX_VALUE,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
            nameResolver,
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
        ZLinkStreamCompression compression,
        ZLinkStreamPacketNameResolver nameResolver) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            64 * 1024,
            Integer.MAX_VALUE,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
            nameResolver,
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
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            64 * 1024,
            Integer.MAX_VALUE,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            ZLinkStreamCompression.LZ4,
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
        boolean skipServerCertificateValidation,
        ZLinkStreamCompression compression) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            64 * 1024,
            Integer.MAX_VALUE,
            heartbeatEnabled,
            heartbeatInterval,
            heartbeatTimeout,
            reconnectEnabled,
            reconnectInitialDelay,
            reconnectMaxDelay,
            reconnectBackoffFactor,
            skipServerCertificateValidation,
            compression,
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
        boolean skipServerCertificateValidation,
        ZLinkStreamPacketNameResolver nameResolver) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
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
            nameResolver,
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
        double reconnectBackoffFactor) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
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
            ZLinkStreamPacketNameResolver.defaultResolver(),
            null);
    }

    public ZLinkStreamConnectorOptions(
        URI endpoint,
        ZLinkStreamDispatchMode dispatchMode,
        Duration requestTimeout,
        int maxReconnectAttempts,
        Duration connectTimeout,
        int maxSendPayloadSize) {
        this(
            endpoint,
            dispatchMode,
            requestTimeout,
            maxReconnectAttempts,
            connectTimeout,
            maxSendPayloadSize,
            true,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(250),
            Duration.ofSeconds(5),
            2.0,
            false);
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
            maxReconnectAttempts,
            Duration.ofSeconds(5),
            64 * 1024);
    }
}
