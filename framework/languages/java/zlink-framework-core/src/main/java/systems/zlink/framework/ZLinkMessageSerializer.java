package systems.zlink.framework;

import java.util.Objects;

/**
 * Payload serializer used by framework handlers and connector helpers.
 */
public interface ZLinkMessageSerializer {
    <T> ZLinkEncodedPayload serialize(T value);

    /**
     * Serializes a value whose declared message type can be broader than its runtime class.
     * Implementations that do not distinguish declared types may use the default behavior.
     */
    default <T> ZLinkEncodedPayload serialize(T value, Class<?> declaredType) {
        Objects.requireNonNull(declaredType, "declaredType");
        return serialize(value);
    }

    <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type);

    default void prepare(Class<?> type) {
    }
}
