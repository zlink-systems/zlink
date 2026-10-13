package systems.zlink.framework.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.messaging.ZLinkOwnedMessageSerializer;

public final class ZLinkMessagePayloads {
    private ZLinkMessagePayloads() {}

    public static ZLinkEncodedPayload encoded(Message message) {
        return ZLinkEncodedPayload.from(message.toByteArray());
    }

    public static Message message(ZLinkEncodedPayload payload) {
        return Message.from(payload.bytes());
    }

    public static Message message(ZLinkMessage message, ZLinkMessageSerializer serializer) {
        return message(message.toEncodedPayload(serializer));
    }

    public static <T> T deserialize(
            ZLinkMessageSerializer serializer, Message message, Class<T> type) {
        var selected = ZLinkCodecRegistration.serializerForPayloadType(serializer, type);
        return selected instanceof ZLinkOwnedMessageSerializer owned
                ? owned.deserializeBorrowed(message, type)
                : selected.deserialize(encoded(message), type);
    }
}
