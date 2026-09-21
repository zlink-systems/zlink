package systems.zlink.stream.connector;

import systems.zlink.contracts.messaging.Message;

import java.util.Map;
import java.util.Objects;

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
                requireCodec(payload.codec()));
    }

    byte[] encode(ZLinkStreamEncodedPayload payload, boolean compress) {
        byte[] body = drainPayload(payload);
        if (!compress) {
            requireWithinSendLimit(body);
            return body;
        }
        ZLinkStreamCompressionCodec codec = configuration.transport().compressionCodec();
        if (codec == null) {
            //  Common connector spec §8: with compression None a send that
            //  asks to compress fails. §9 files that under CompressionFailed
            //  because only this one send operation fails.
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.COMPRESSION_FAILED, "compression codec is not configured");
        }
        byte[] compressed;
        try {
            compressed = codec.compress(body);
        } catch (ZLinkStreamException alreadyCoded) {
            throw alreadyCoded;
        } catch (RuntimeException failure) {
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.COMPRESSION_FAILED,
                    "stream payload compression failed",
                    failure);
        }
        requireWithinSendLimit(compressed);
        return compressed;
    }

    byte[] decode(ZLinkStreamWireProtocol.Header header, byte[] payload) {
        if ((header.flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED) == 0) {
            return payload;
        }
        ZLinkStreamCompressionCodec codec = configuration.transport().compressionCodec();
        if (codec == null) {
            //  Common connector spec §8: with compression None an inbound
            //  frame that carries the compressed flag is rejected as
            //  DecompressionFailed.
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.DECOMPRESSION_FAILED,
                    "compression codec is not configured");
        }
        byte[] decoded;
        try {
            decoded = codec.decompress(payload, configuration.limits().receivePayload());
        } catch (ZLinkStreamException alreadyCoded) {
            throw alreadyCoded;
        } catch (RuntimeException failure) {
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.DECOMPRESSION_FAILED,
                    "stream payload decompression failed",
                    failure);
        }
        if (decoded.length > configuration.limits().receivePayload()) {
            throw ZLinkStreamException.of(
                    ZLinkStreamErrorCode.DECOMPRESSION_FAILED,
                    "decompressed stream payload exceeds maximum stream payload size");
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

    private static ZLinkStreamCodec requireCodec(ZLinkStreamCodec codec) {
        if (codec == null) {
            throw ZLinkStreamException.validationFailed("payload codec is required");
        }
        return codec;
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
            //  Common connector spec §9: exceeding the send payload limit is
            //  pre-send validation, so it is ValidationFailed, not
            //  FrameTooLarge (which is the receive-side limit).
            throw ZLinkStreamException.validationFailed("payload exceeds max payload size");
        }
    }
}
