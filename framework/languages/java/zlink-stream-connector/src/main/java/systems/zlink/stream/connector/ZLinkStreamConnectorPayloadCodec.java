package systems.zlink.stream.connector;

import java.util.Map;
import java.util.Objects;
import systems.zlink.contracts.messaging.Message;

final class ZLinkStreamConnectorPayloadCodec {
    private final ZLinkStreamConnectorConfiguration configuration;

    ZLinkStreamConnectorPayloadCodec(ZLinkStreamConnectorConfiguration configuration) {
        this.configuration = Objects.requireNonNull(configuration, "configuration");
    }

    ZLinkStreamEncodedPayload copy(ZLinkStreamEncodedPayload payload) {
        Objects.requireNonNull(payload, "payload");
        return new ZLinkStreamEncodedPayload(
            DefaultZLinkStreamConnector.validatePacketName(payload.packetName()),
            Message.from(payload.payload()),
            Map.copyOf(payload.metadata()),
            Objects.requireNonNull(payload.codec(), "codec"));
    }

    byte[] encode(ZLinkStreamEncodedPayload payload, boolean compress) {
        byte[] body = drainPayload(payload);
        if (!compress) {
            requireWithinSendLimit(body);
            return body;
        }
        ZLinkStreamCompressionCodec codec = configuration.transport().compressionCodec();
        if (codec == null) {
            throw new IllegalStateException("compression codec is not configured");
        }
        byte[] compressed = codec.compress(body);
        requireWithinSendLimit(compressed);
        return compressed;
    }

    byte[] decode(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        if ((header.flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED) == 0) {
            return payload;
        }
        ZLinkStreamCompressionCodec codec = configuration.transport().compressionCodec();
        if (codec == null) {
            throw new IllegalStateException("compression codec is not configured");
        }
        byte[] decoded = codec.decompress(payload, configuration.limits().receivePayload());
        if (decoded.length > configuration.limits().receivePayload()) {
            throw new IllegalStateException("decompressed stream payload exceeds maximum stream payload size");
        }
        return decoded;
    }

    static int toWireCodec(ZLinkStreamCodec codec) {
        return switch (Objects.requireNonNull(codec, "codec")) {
            case RAW -> ZLinkStreamWireProtocol.CODEC_RAW;
            case JSON -> ZLinkStreamWireProtocol.CODEC_JSON;
            case MESSAGE_PACK -> ZLinkStreamWireProtocol.CODEC_MESSAGE_PACK;
            case PROTOBUF -> ZLinkStreamWireProtocol.CODEC_PROTOBUF;
        };
    }

    static ZLinkStreamCodec fromWireCodec(int codec) {
        return switch (codec) {
            case ZLinkStreamWireProtocol.CODEC_RAW -> ZLinkStreamCodec.RAW;
            case ZLinkStreamWireProtocol.CODEC_JSON -> ZLinkStreamCodec.JSON;
            case ZLinkStreamWireProtocol.CODEC_MESSAGE_PACK -> ZLinkStreamCodec.MESSAGE_PACK;
            case ZLinkStreamWireProtocol.CODEC_PROTOBUF -> ZLinkStreamCodec.PROTOBUF;
            default -> throw new IllegalArgumentException("unknown stream codec");
        };
    }

    private static byte[] drainPayload(ZLinkStreamEncodedPayload payload) {
        try {
            return payload.payload().toByteArray();
        } finally {
            payload.payload().close();
        }
    }

    private void requireWithinSendLimit(byte[] payload) {
        if (payload.length > configuration.limits().sendPayload()) {
            throw new IllegalArgumentException("payload exceeds max payload size");
        }
    }
}
