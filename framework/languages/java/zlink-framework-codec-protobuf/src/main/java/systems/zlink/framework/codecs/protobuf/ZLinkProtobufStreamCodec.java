package systems.zlink.framework.codecs.protobuf;

import com.google.protobuf.MessageLite;

import systems.zlink.stream.connector.ZLinkStreamCodec;
import systems.zlink.stream.connector.ZLinkStreamEncodedPayload;
import systems.zlink.stream.connector.ZLinkStreamTypedCodec;

import java.util.Map;

enum ZLinkProtobufStreamCodec implements ZLinkStreamTypedCodec {
    INSTANCE;

    @Override
    public <T> ZLinkStreamEncodedPayload encode(String packetName, T value) {
        return new ZLinkStreamEncodedPayload(
                packetName,
                value instanceof MessageLite
                        ? ZLinkProtobufMessageSerializer.INSTANCE.serializeOwned(value)
                        : ZLinkProtobufPayloads.encodeStream(value),
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
            return ZLinkProtobufMessageSerializer.INSTANCE.deserializeBorrowed(
                    payload.payload(), type);
        }
        return ZLinkProtobufPayloads.decodeStreamFallback(payload.payload(), type);
    }
}
