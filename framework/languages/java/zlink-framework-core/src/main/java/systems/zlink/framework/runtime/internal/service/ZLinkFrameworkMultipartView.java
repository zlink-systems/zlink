package systems.zlink.framework.runtime.internal.service;

import java.nio.ByteBuffer;
import java.util.AbstractList;
import java.util.List;
import systems.zlink.contracts.messaging.Message;

/** A retained multipart frame whose typed parts are materialized on first use. */
public final class ZLinkFrameworkMultipartView extends AbstractList<Message>
    implements AutoCloseable {
    private final List<ByteBuffer> parts;
    private final Message[] materialized;
    private boolean detached;
    private boolean closed;

    ZLinkFrameworkMultipartView(List<ByteBuffer> parts) {
        this.parts = List.copyOf(parts);
        this.materialized = new Message[parts.size()];
    }

    @Override
    public synchronized Message get(int index) {
        if (closed) {
            throw new IllegalStateException("multipart view is closed");
        }
        Message value = materialized[index];
        if (value == null) {
            value = Message.from(parts.get(index));
            materialized[index] = value;
        }
        return value;
    }

    @Override
    public int size() {
        return parts.size();
    }

    synchronized List<Message> detachAll() {
        try {
            for (int index = 0; index < materialized.length; index++) {
                get(index);
            }
            detached = true;
            return List.of(materialized);
        } catch (RuntimeException failure) {
            close();
            throw failure;
        }
    }

    @Override
    public synchronized void close() {
        if (closed || detached) {
            return;
        }
        closed = true;
        for (Message part : materialized) {
            if (part != null) {
                part.close();
            }
        }
    }
}
