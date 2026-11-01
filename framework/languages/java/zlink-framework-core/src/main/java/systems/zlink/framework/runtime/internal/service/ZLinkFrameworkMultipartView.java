package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.runtime.internal.messaging.ZLinkEncodedPayloadAccess;

import java.nio.ByteBuffer;
import java.util.AbstractList;
import java.util.List;
import java.util.Objects;

/**
 * A retained multipart frame whose typed parts are materialized on first use.
 *
 * <p>One record owner accesses this view at a time; ownership handoff publishes its state.
 */
public final class ZLinkFrameworkMultipartView extends AbstractList<Message>
        implements AutoCloseable {
    private final Object[] parts;
    // Keeps borrowed storage alive; outbound envelopes retain the moved payload owner.
    private final AutoCloseable frame;
    private boolean detached;
    private boolean closed;

    /** Takes exclusive ownership of the codec's part slots and of {@code frame}, if any. */
    ZLinkFrameworkMultipartView(Object[] parts, AutoCloseable frame) {
        this.parts = parts;
        this.frame = frame;
    }

    @Override
    public Message get(int index) {
        Object value = partValue(index);
        Message materialized;
        if (value instanceof ByteBuffer bytes) {
            materialized = Message.from(bytes);
        } else if (value instanceof byte[] bytes) {
            materialized = Message.from(bytes);
        } else {
            return (Message) value;
        }
        parts[index] = materialized;
        return materialized;
    }

    /**
     * Outbound envelope parts: the header bytes are retained directly until the transport writes or
     * materializes them. They may be shared with other envelopes, so they are only ever read and
     * are exposed through read-only views. Closing the view releases {@code body}.
     */
    public static ZLinkFrameworkMultipartView encoded(byte[] header, Message body) {
        return new ZLinkFrameworkMultipartView(
                new Object[] {
                    Objects.requireNonNull(header, "header"), Objects.requireNonNull(body, "body")
                },
                null);
    }

    /** Transfers the single retained payload owner into its outbound envelope. */
    public static ZLinkFrameworkMultipartView encoded(
            byte[] header, ZLinkFrameworkMultipartView body) {
        var envelope =
                new ZLinkFrameworkMultipartView(
                        new Object[] {Objects.requireNonNull(header, "header"), body.partValue(0)},
                        body);
        body.parts[0] = null;
        return envelope;
    }

    public static ZLinkFrameworkMultipartView encoded(ZLinkEncodedPayload body) {
        return new ZLinkFrameworkMultipartView(
                new Object[] {ZLinkEncodedPayloadAccess.borrow(body)}, null);
    }

    /** Takes ownership of one already materialized outbound message. */
    public static ZLinkFrameworkMultipartView owned(Message body) {
        return new ZLinkFrameworkMultipartView(
                new Object[] {Objects.requireNonNull(body, "body")}, null);
    }

    /** Snapshots caller-owned bytes into retained direct storage without a heap byte-array copy. */
    public static ZLinkFrameworkMultipartView snapshot(byte[] body) {
        Objects.requireNonNull(body, "body");
        ByteBuffer snapshot = ByteBuffer.allocateDirect(body.length);
        snapshot.put(body).flip();
        return new ZLinkFrameworkMultipartView(new Object[] {snapshot}, null);
    }

    /**
     * Reads one part without materializing it as a binding message. The view stays valid while this
     * record's frame owner is alive; a part already materialized is read in place.
     */
    public static ByteBuffer buffer(List<Message> parts, int index) {
        Object value = partValue(parts, index);
        if (value instanceof ByteBuffer bytes) return bytes.asReadOnlyBuffer();
        if (value instanceof byte[] bytes) return ByteBuffer.wrap(bytes).asReadOnlyBuffer();
        return ((Message) value).dataBuffer();
    }

    /** The byte length of one part, without materializing it. */
    static int partSize(List<Message> parts, int index) {
        Object value = partValue(parts, index);
        if (value instanceof byte[] bytes) {
            return bytes.length;
        }
        return value instanceof ByteBuffer bytes ? bytes.remaining() : ((Message) value).size();
    }

    /** Writes one part once into {@code target}, without materializing it. */
    static void writePart(List<Message> parts, int index, ByteBuffer target) {
        Object value = partValue(parts, index);
        if (value instanceof byte[] bytes) {
            target.put(bytes);
        } else if (value instanceof ByteBuffer bytes) {
            target.put(bytes.duplicate());
        } else {
            ((Message) value).copyTo(target);
        }
    }

    private static Object partValue(List<Message> parts, int index) {
        return parts instanceof ZLinkFrameworkMultipartView view
                ? view.partValue(index)
                : parts.get(index);
    }

    private Object partValue(int index) {
        if (closed) {
            throw new IllegalStateException("multipart view is closed");
        }
        return parts[index];
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
            Message.closeAll(parts);
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
            closeOwner();
            return result;
        } catch (RuntimeException failure) {
            close();
            throw failure;
        }
    }

    /** Transfers the single part to a binding call, materializing it only at that boundary. */
    public Message detachFirst() {
        if (parts.length != 1) {
            throw new IllegalStateException("multipart view does not contain exactly one part");
        }
        Message message = get(0);
        parts[0] = null;
        detached = true;
        closeOwner();
        return message;
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
            java.util.Arrays.fill(parts, null);
            closeOwner();
        }
    }

    private void closeOwner() {
        if (frame != null) {
            try {
                frame.close();
            } catch (RuntimeException | Error failure) {
                throw failure;
            } catch (Exception failure) {
                throw new IllegalStateException(
                        "retained multipart owner could not be closed", failure);
            }
        }
    }
}
