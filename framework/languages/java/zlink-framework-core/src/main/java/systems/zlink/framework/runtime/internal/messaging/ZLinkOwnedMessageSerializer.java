package systems.zlink.framework.runtime.internal.messaging;

import systems.zlink.contracts.messaging.Message;

/** Codec output transferred directly to the binding message owner. */
public interface ZLinkOwnedMessageSerializer {
    Message serializeOwned(Object value);

    <T> T deserializeBorrowed(Message message, Class<T> type);
}
