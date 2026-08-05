package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import systems.zlink.contracts.messaging.Message;

/** Accumulates raw STREAM bytes and yields complete framework frames. */
final class ZLinkStreamReceiveBuffer implements AutoCloseable {
    private static final int PREFIX_SIZE = 6;
    private final long maxMessageSize;
    private byte[] buffer = new byte[4096];
    private int offset;
    private int length;
    private boolean closed;

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
            throw new IllegalArgumentException(
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
            consume(Math.toIntExact(totalBytes));
            return new ZLinkStreamInboundFrame(header, payload);
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
}
