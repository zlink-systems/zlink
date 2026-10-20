package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.messaging.Message;

import java.nio.ByteBuffer;
import java.util.AbstractList;
import java.util.List;

/**
 * A retained multipart frame whose typed parts are materialized on first use.
 *
 * <p>One record owner accesses this view at a time; ownership handoff publishes its state.
 */
public final class ZLinkFrameworkMultipartView extends AbstractList<Message>
        implements AutoCloseable {
    private final Object[] parts;
    private boolean detached;
    private boolean closed;

    /** Takes exclusive ownership of the codec's part slots. */
    ZLinkFrameworkMultipartView(Object[] parts) {
        this.parts = parts;
    }

    @Override
    public Message get(int index) {
        if (closed) {
            throw new IllegalStateException("multipart view is closed");
        }
        Object value = parts[index];
        if (value instanceof ByteBuffer bytes) {
            Message materialized = Message.from(bytes);
            parts[index] = materialized;
            return materialized;
        }
        return (Message) value;
    }

    @Override
    public int size() {
        return parts.length;
    }

    @SuppressWarnings("unchecked")
    List<Message> detachAll() {
        try {
            for (int index = 0; index < parts.length; index++) {
                get(index);
            }
            // Every slot now owns a Message; List.of publishes an immutable list of them.
            List<Message> result = (List<Message>) (List<?>) List.of(parts);
            detached = true;
            return result;
        } catch (RuntimeException failure) {
            close();
            throw failure;
        }
    }

    @Override
    public void close() {
        if (closed || detached) {
            return;
        }
        closed = true;
        for (Object part : parts) {
            if (part instanceof Message message) {
                message.close();
            }
        }
    }
}
