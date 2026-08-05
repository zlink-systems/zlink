package systems.zlink.framework.streams;

import systems.zlink.framework.runtime.streams.ZLinkStreamPayloadCompression;

public final class ZLinkStreamCompressionCodecs {
    private static final ZLinkStreamCompressionCodec LZ4 = new ZLinkStreamCompressionCodec() {
        @Override
        public byte[] compress(byte[] payload) {
            return ZLinkStreamPayloadCompression.lz4Pickle(payload);
        }

        @Override
        public byte[] decompress(byte[] payload, int maxDecompressedSize) {
            return ZLinkStreamPayloadCompression.lz4Unpickle(payload, maxDecompressedSize);
        }
    };

    private ZLinkStreamCompressionCodecs() {
    }

    public static ZLinkStreamCompressionCodec lz4() {
        return LZ4;
    }
}
