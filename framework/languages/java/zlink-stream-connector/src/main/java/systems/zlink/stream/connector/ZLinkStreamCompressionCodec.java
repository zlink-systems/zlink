package systems.zlink.stream.connector;

public interface ZLinkStreamCompressionCodec {
    byte[] compress(byte[] payload);

    byte[] decompress(byte[] payload, int maxDecompressedSize);
}
