package systems.zlink.framework;

/**
 * Payload serializer used by framework handlers and connector helpers.
 */
public interface ZLinkMessageSerializer {
    <T> ZLinkEncodedPayload serialize(T value);

    <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type);

    default void prepare(Class<?> type) {
    }
}
