package systems.zlink.framework.codecs.protobuf;

import java.util.Map;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamTypedCodec;

enum ZLinkProtobufStreamCodec implements ZLinkStreamTypedCodec {
    INSTANCE;

    @Override
    public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(ZLinkProtobufPayloads.encodeStream(value)),
            Map.of(),
            ZLinkStreamCodec.PROTOBUF);
    }

    @Override
    public <T> T decode(ZLinkStreamEncodedPayload payload, Class<T> type) {
        if (payload.codec() != ZLinkStreamCodec.PROTOBUF) {
            throw new IllegalArgumentException(
                "stream payload codec is " + payload.codec() + ", not PROTOBUF");
        }
        if (ZLinkProtobufMessageSerializer.canSerialize(type)) {
            return ZLinkProtobufMessageSerializer.INSTANCE.deserialize(
                ZLinkEncodedPayload.from(payload.payload().toByteArray()),
                type);
        }
        return ZLinkProtobufPayloads.decodeStreamFallback(
            payload.payload().toByteArray(),
            type);
    }
}
