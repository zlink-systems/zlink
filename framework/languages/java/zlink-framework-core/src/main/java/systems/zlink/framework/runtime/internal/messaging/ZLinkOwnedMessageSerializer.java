package systems.zlink.framework.runtime.internal.messaging;

import systems.zlink.contracts.messaging.Message;

import java.nio.ByteBuffer;

/** Codec output transferred directly to the binding message owner. */
public interface ZLinkOwnedMessageSerializer {
    Message serializeOwned(Object value);

    /** Decodes bytes the caller keeps alive for the duration of this call. */
    <T> T deserializeBorrowed(ByteBuffer payload, Class<T> type);
}
