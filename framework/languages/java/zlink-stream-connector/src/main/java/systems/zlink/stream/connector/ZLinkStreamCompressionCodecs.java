package systems.zlink.stream.connector;

public final class ZLinkStreamCompressionCodecs {
    private static final ZLinkStreamCompressionCodec LZ4 = new ZLinkStreamCompressionCodec() {
        @Override
        public byte[] compress(byte[] payload) {
            return ZLinkStreamLz4Pickler.pickle(payload);
        }

        @Override
        public byte[] decompress(byte[] payload, int maxDecompressedSize) {
            return ZLinkStreamLz4Pickler.unpickle(payload, maxDecompressedSize);
        }
    };

    private ZLinkStreamCompressionCodecs() {
    }

    public static ZLinkStreamCompressionCodec lz4() {
        return LZ4;
    }
}
