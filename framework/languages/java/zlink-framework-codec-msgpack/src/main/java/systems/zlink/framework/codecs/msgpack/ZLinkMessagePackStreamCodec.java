package systems.zlink.framework.codecs.msgpack;

import java.util.Map;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.stream.connector.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamTypedCodec;

enum ZLinkMessagePackStreamCodec implements ZLinkStreamTypedCodec {
    INSTANCE;

    @Override
    public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(ZLinkMessagePackPayloads.encode(value, "stream payload")),
            Map.of(),
            ZLinkStreamCodec.MESSAGE_PACK);
    }

    @Override
    public <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
        if (payload.codec() != ZLinkStreamCodec.MESSAGE_PACK) {
            throw new IllegalArgumentException(
                "stream payload codec is " + payload.codec() + ", not MESSAGE_PACK");
        }
        return ZLinkMessagePackPayloads.decode(
            payload.payload().toByteArray(),
            type,
            "stream payload");
    }
}
