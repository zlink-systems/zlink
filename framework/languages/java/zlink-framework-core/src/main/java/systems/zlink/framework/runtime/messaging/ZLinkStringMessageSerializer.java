package systems.zlink.framework.runtime.messaging;

import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;

public final class ZLinkStringMessageSerializer implements ZLinkMessageSerializer {
    @Override
    public <T> ZLinkEncodedPayload serialize(T value) {
        if (value instanceof Message message) {
            return ZLinkEncodedPayload.from(message.toByteArray());
        }
        if (value instanceof byte[] bytes) {
            return ZLinkEncodedPayload.from(bytes);
        }
        return ZLinkEncodedPayload.from(String.valueOf(value).getBytes(StandardCharsets.UTF_8));
    }

    @Override
    public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
        byte[] bytes = payload.bytes();
        if (type == Message.class) {
            return type.cast(Message.from(bytes));
        }
        if (type == byte[].class) {
            return type.cast(bytes);
        }
        if (type == String.class) {
            return type.cast(new String(bytes, StandardCharsets.UTF_8));
        }
        throw new IllegalArgumentException("unsupported message type: " + type.getName());
    }
}
