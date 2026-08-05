package systems.zlink.framework.runtime.streams;

public final class ZLinkStreamPayloadCompression {
    private ZLinkStreamPayloadCompression() {
    }

    public static byte[] lz4Pickle(byte[] source) {
        return ZLinkStreamLz4Pickler.pickle(source);
    }

    public static byte[] lz4Unpickle(byte[] source) {
        return ZLinkStreamLz4Pickler.unpickle(source);
    }

    public static byte[] lz4Unpickle(byte[] source, int maxDecompressedSize) {
        return ZLinkStreamLz4Pickler.unpickle(source, maxDecompressedSize);
    }
}
