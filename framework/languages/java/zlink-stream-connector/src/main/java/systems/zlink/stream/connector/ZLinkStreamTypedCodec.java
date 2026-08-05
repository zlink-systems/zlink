package systems.zlink.stream.connector;

public interface ZLinkStreamTypedCodec {
    <T> ZLinkStreamEncodedPayload encode(String packetName, T value);

    <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type);
}
