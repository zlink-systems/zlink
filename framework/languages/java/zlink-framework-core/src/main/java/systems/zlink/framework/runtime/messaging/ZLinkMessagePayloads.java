package systems.zlink.framework.runtime.messaging;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.messaging.ZLinkMessage;

public final class ZLinkMessagePayloads {
    private ZLinkMessagePayloads() {
    }

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
        ZLinkMessageSerializer serializer,
        Message message,
        Class<T> type) {
        return serializer.deserialize(encoded(message), type);
    }
}
