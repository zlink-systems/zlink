package systems.zlink.framework.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;

import java.nio.ByteBuffer;

public final class ZLinkMessagePayloads {
    private ZLinkMessagePayloads() {}

    public static ZLinkEncodedPayload encoded(Message message) {
        return ZLinkEncodedPayload.from(message.toByteArray());
    }

    public static Message message(ZLinkEncodedPayload payload) {
        return Message.from(payload.bytes());
    }

    public static Message message(
            ZLinkMessageSerializer serializer, Object value, Class<?> declaredType) {
        var selected = ZLinkCodecRegistration.serializerForPayloadType(serializer, declaredType);
        return selected instanceof ZLinkOwnedMessageSerializer owned
                ? owned.serializeOwned(value)
                : message(selected.serialize(value, declaredType));
    }

    public static Message message(ZLinkMessage message, ZLinkMessageSerializer serializer) {
        return message(message.toEncodedPayload(serializer));
    }

    /** Decodes bytes the caller keeps alive for the duration of this call. */
    public static <T> T deserialize(
            ZLinkMessageSerializer serializer, ByteBuffer payload, Class<T> type) {
        var selected = ZLinkCodecRegistration.serializerForPayloadType(serializer, type);
        if (selected instanceof ZLinkOwnedMessageSerializer owned) {
            return owned.deserializeBorrowed(payload, type);
        }
        byte[] bytes = new byte[payload.remaining()];
        payload.duplicate().get(bytes);
        return selected.deserialize(ZLinkEncodedPayload.from(bytes), type);
    }
}
