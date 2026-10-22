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
    // The received frame the unmaterialized parts read from, when this view owns it.
    private final Message frame;
    private boolean detached;
    private boolean closed;

    /** Takes exclusive ownership of the codec's part slots and of {@code frame}, if any. */
    ZLinkFrameworkMultipartView(Object[] parts, Message frame) {
        this.parts = parts;
        this.frame = frame;
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

    /**
     * Reads one part without materializing it as a binding message. The view stays valid while this
     * record's frame owner is alive; a part already materialized is read in place.
     */
    public static ByteBuffer buffer(List<Message> parts, int index) {
        if (parts instanceof ZLinkFrameworkMultipartView view) {
            if (view.closed) {
                throw new IllegalStateException("multipart view is closed");
            }
            if (view.parts[index] instanceof ByteBuffer bytes) {
                return bytes.duplicate();
            }
        }
        return parts.get(index).dataBuffer();
    }

    /** Releases received parts; a retained list releases its storage once, by itself. */
    public static void closeAll(List<Message> parts) {
        if (parts instanceof AutoCloseable retained) {
            try {
                retained.close();
            } catch (RuntimeException failure) {
                throw failure;
            } catch (Exception failure) {
                throw new IllegalStateException(
                        "retained message parts could not be closed", failure);
            }
        } else {
            parts.forEach(Message::close);
        }
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
            if (frame != null) frame.close();
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
        try {
            for (Object part : parts) {
                if (part instanceof Message message) {
                    message.close();
                }
            }
        } finally {
            if (frame != null) frame.close();
        }
    }
}
