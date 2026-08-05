package systems.zlink.framework.runtime.streams;

import java.util.EnumSet;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.streams.ZLinkStreamCompressionCodec;

final class ZLinkStreamPayloadCodec {
    private static final int DEFAULT_MAX_DECOMPRESSED_PAYLOAD_SIZE = 64 * 1024;

    private ZLinkStreamPayloadCodec() {
    }

    static Encoded encode(
        Message payload,
        boolean compress,
        ZLinkStreamCompressionCodec compressionCodec) {
        byte[] bytes = payload.toByteArray();
        if (!compress) {
            return new Encoded(bytes, EnumSet.noneOf(ZLinkStreamHeaderFlag.class));
        }
        if (compressionCodec == null) {
            throw new IllegalStateException("compression codec is not configured");
        }
        return new Encoded(
            compressionCodec.compress(bytes),
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
    }

    static byte[] decode(
        ZLinkStreamHeader header,
        Message payload,
        ZLinkStreamCompressionCodec compressionCodec) {
        byte[] bytes = payload.toByteArray();
        if (!header.flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED)) {
            return bytes;
        }
        if (compressionCodec == null) {
            throw new IllegalStateException("compression codec is not configured");
        }
        byte[] decoded = compressionCodec.decompress(bytes, DEFAULT_MAX_DECOMPRESSED_PAYLOAD_SIZE);
        if (decoded.length > DEFAULT_MAX_DECOMPRESSED_PAYLOAD_SIZE) {
            throw new IllegalStateException("decompressed stream payload exceeds maximum stream payload size");
        }
        return decoded;
    }

    record Encoded(
        byte[] payload,
        EnumSet<ZLinkStreamHeaderFlag> flags) {
    }
}
