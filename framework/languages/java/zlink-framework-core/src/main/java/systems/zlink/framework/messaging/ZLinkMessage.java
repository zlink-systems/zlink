package systems.zlink.framework.messaging;

import java.util.Objects;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;

public final class ZLinkMessage {
    private static final ZLinkEncodedPayload EMPTY_PAYLOAD =
        ZLinkEncodedPayload.from(new byte[0]);

    private final Object value;
    private final ZLinkEncodedPayload encodedPayload;
    private final ZLinkMessageSerializer serializer;
    private final boolean empty;

    private ZLinkMessage(
        Object value,
        ZLinkEncodedPayload encodedPayload,
        ZLinkMessageSerializer serializer,
        boolean empty) {
        this.value = value;
        this.encodedPayload = encodedPayload;
        this.serializer = serializer;
        this.empty = empty;
    }

    public static ZLinkMessage empty() {
        return new ZLinkMessage(null, EMPTY_PAYLOAD, null, true);
    }

    public static ZLinkMessage of(Object value) {
        if (value instanceof ZLinkMessage message) {
            return message;
        }
        return new ZLinkMessage(
            Objects.requireNonNull(value, "value"), null, null, false);
    }

    public static ZLinkMessage fromEncoded(ZLinkEncodedPayload payload, ZLinkMessageSerializer serializer) {
        Objects.requireNonNull(payload, "payload");
        Objects.requireNonNull(serializer, "serializer");
        return new ZLinkMessage(
            null,
            payload,
            serializer,
            payload.bytes().length == 0);
    }

    public boolean isEmpty() {
        return empty;
    }

    public <T> T decode(Class<T> type) {
        Objects.requireNonNull(type, "type");
        if (value != null) {
            if (type.isInstance(value)) {
                return type.cast(value);
            }
            ZLinkEncodedPayload encoded = serializerForEncode().serialize(value);
            return serializerForEncode().deserialize(encoded, type);
        }
        return serializerForDecode().deserialize(encodedPayload, type);
    }

    public ZLinkEncodedPayload toEncodedPayload(ZLinkMessageSerializer serializer) {
        Objects.requireNonNull(serializer, "serializer");
        if (value != null) {
            return serializer.serialize(value);
        }
        return encodedPayload;
    }

    private ZLinkMessageSerializer serializerForDecode() {
        if (serializer == null) {
            throw new IllegalStateException("message does not have a codec registry for decode");
        }
        return serializer;
    }

    private ZLinkMessageSerializer serializerForEncode() {
        if (serializer != null) {
            return serializer;
        }
        throw new IllegalStateException("message does not have a codec registry for re-encode");
    }
}
