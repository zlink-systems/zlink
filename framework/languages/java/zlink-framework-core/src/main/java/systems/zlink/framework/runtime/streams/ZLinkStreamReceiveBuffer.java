package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import java.util.ArrayDeque;
import java.util.LinkedHashSet;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;

/** Accumulates raw STREAM bytes and yields complete framework frames. */
final class ZLinkStreamReceiveBuffer implements AutoCloseable {
    private static final int PREFIX_SIZE = 6;
    private final long maxMessageSize;
    private byte[] buffer = new byte[4096];
    private int offset;
    private int length;
    private boolean closed;
    private final ArrayDeque<RetainedSegment> retainedSegments = new ArrayDeque<>();

    ZLinkStreamReceiveBuffer(long maxMessageSize) {
        if (maxMessageSize < 0) {
            throw new IllegalArgumentException("maxMessageSize must not be negative");
        }
        this.maxMessageSize = maxMessageSize;
    }

    void append(byte[] bytes) {
        ensureOpen();
        if (bytes == null || bytes.length == 0) {
            return;
        }
        int required;
        try {
            required = Math.addExact(length, bytes.length);
        } catch (ArithmeticException overflow) {
            throw new IllegalArgumentException("STREAM receive buffer is too large", overflow);
        }
        ensureCapacity(required);
        System.arraycopy(bytes, 0, buffer, offset + length, bytes.length);
        length = required;
    }

    void append(
        java.util.List<Message> parts,
        ZLinkBackendStreamReceived received) {
        ensureOpen();
        RetainedOwner owner = new RetainedOwner(received);
        boolean retained = false;
        try {
            for (Message part : parts) {
                byte[] bytes = part.toByteArray();
                append(bytes);
                if (bytes.length > 0) {
                    retainedSegments.addLast(new RetainedSegment(bytes.length, owner));
                    owner.addBufferedSegment();
                    retained = true;
                }
            }
        } catch (RuntimeException | Error failure) {
            if (!retained) {
                owner.closeNow();
            }
            throw failure;
        }
        if (!retained) {
            owner.closeNow();
        }
    }

    ZLinkStreamInboundFrame tryTakeFrame() {
        ensureOpen();
        if (length < PREFIX_SIZE) {
            return null;
        }

        ByteBuffer input = ByteBuffer.wrap(buffer, offset, length);
        int headerSize = Short.toUnsignedInt(input.getShort());
        long payloadSize = Integer.toUnsignedLong(input.getInt());
        long messageSize;
        long totalBytes;
        try {
            messageSize = Math.addExact((long) headerSize, payloadSize);
            totalBytes = Math.addExact(PREFIX_SIZE, messageSize);
        } catch (ArithmeticException overflow) {
            throw new IllegalArgumentException("STREAM frame length overflows", overflow);
        }
        if (maxMessageSize > 0 && messageSize > maxMessageSize) {
            throw new ZLinkStreamMessageTooLargeException(
                "STREAM frame exceeds MaxMessageSize");
        }
        if (totalBytes > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                "STREAM frame exceeds the supported size");
        }
        if (length < totalBytes) {
            return null;
        }

        int bodyOffset = offset + PREFIX_SIZE;
        Message header = Message.from(buffer, bodyOffset, headerSize);
        try {
            Message payload = Message.from(
                buffer,
                bodyOffset + headerSize,
                Math.toIntExact(payloadSize));
            Runnable terminalRelease = consumeRetained(Math.toIntExact(totalBytes));
            consume(Math.toIntExact(totalBytes));
            return new ZLinkStreamInboundFrame(header, payload, terminalRelease);
        } catch (RuntimeException failure) {
            header.close();
            throw failure;
        }
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        buffer = new byte[0];
        offset = 0;
        length = 0;
        while (!retainedSegments.isEmpty()) {
            RetainedSegment segment = retainedSegments.removeFirst();
            segment.owner.removeBufferedSegment();
        }
    }

    private void ensureOpen() {
        if (closed) {
            throw new IllegalStateException("STREAM receive buffer is closed");
        }
    }

    private void ensureCapacity(int required) {
        if (required <= buffer.length - offset) {
            return;
        }
        if (offset > 0) {
            System.arraycopy(buffer, offset, buffer, 0, length);
            offset = 0;
            if (required <= buffer.length) {
                return;
            }
        }
        int capacity = Math.max(1, buffer.length);
        while (capacity < required) {
            int next = capacity << 1;
            if (next <= capacity) {
                capacity = required;
                break;
            }
            capacity = next;
        }
        byte[] expanded = new byte[capacity];
        System.arraycopy(buffer, offset, expanded, 0, length);
        buffer = expanded;
        offset = 0;
    }

    private void consume(int consumed) {
        offset += consumed;
        length -= consumed;
        if (length == 0) {
            offset = 0;
            return;
        }
        if (offset >= buffer.length / 2) {
            System.arraycopy(buffer, offset, buffer, 0, length);
            offset = 0;
        }
    }

    private Runnable consumeRetained(int consumed) {
        int remaining = consumed;
        Set<RetainedOwner> owners = new LinkedHashSet<>();
        while (remaining > 0) {
            RetainedSegment segment = retainedSegments.peekFirst();
            if (segment == null) {
                break;
            }
            int part = Math.min(remaining, segment.remainingBytes);
            segment.remainingBytes -= part;
            remaining -= part;
            if (owners.add(segment.owner)) {
                segment.owner.claimFrame();
            }
            if (segment.remainingBytes == 0) {
                retainedSegments.removeFirst();
                segment.owner.removeBufferedSegment();
            }
        }
        AtomicBoolean released = new AtomicBoolean();
        return () -> {
            if (!released.compareAndSet(false, true)) {
                return;
            }
            for (RetainedOwner owner : owners) {
                owner.releaseFrame();
            }
        };
    }

    private static final class RetainedSegment {
        private int remainingBytes;
        private final RetainedOwner owner;

        private RetainedSegment(int remainingBytes, RetainedOwner owner) {
            this.remainingBytes = remainingBytes;
            this.owner = owner;
        }
    }

    private static final class RetainedOwner {
        private final ZLinkBackendStreamReceived received;
        private int bufferedSegments;
        private int frameClaims;
        private boolean closed;

        private RetainedOwner(ZLinkBackendStreamReceived received) {
            this.received = received;
        }

        private synchronized void addBufferedSegment() {
            if (closed) {
                throw new IllegalStateException("STREAM retained owner is closed");
            }
            bufferedSegments++;
        }

        private synchronized void removeBufferedSegment() {
            if (bufferedSegments <= 0) {
                throw new IllegalStateException(
                    "STREAM retained segment accounting underflow");
            }
            bufferedSegments--;
            tryCloseLocked();
        }

        private synchronized void claimFrame() {
            if (closed) {
                throw new IllegalStateException("STREAM retained owner is closed");
            }
            frameClaims++;
        }

        private synchronized void releaseFrame() {
            if (frameClaims <= 0) {
                throw new IllegalStateException(
                    "STREAM retained frame accounting underflow");
            }
            frameClaims--;
            tryCloseLocked();
        }

        private synchronized void closeNow() {
            if (!closed) {
                closed = true;
                received.close();
            }
        }

        private void tryCloseLocked() {
            if (bufferedSegments == 0 && frameClaims == 0) {
                if (!closed) {
                    closed = true;
                    received.close();
                }
            }
        }
    }
}
