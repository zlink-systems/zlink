/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.contracts.messaging;

import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.internal.ContractAccess;
import io.netty.buffer.ByteBuf;
import java.lang.reflect.Field;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Objects;
import sun.misc.Unsafe;

/**
 * Owns one native zlink frame and exposes canonical copy factories for Java
 * payload inputs.
 *
 * <p>{@code copyOf*} always copies into message-owned storage. Java does not
 * expose borrowed payload wrappers because native queue lifetime is not safely
 * bounded by Java object reachability.
 */
public final class Message implements AutoCloseable {
    private static final Unsafe UNSAFE = UnsafeAccess.get();
    private static final long BYTE_ARRAY_BASE =
        UNSAFE.arrayBaseOffset(byte[].class);
    private static final boolean NATIVE_LITTLE_ENDIAN =
        ByteOrder.nativeOrder() == ByteOrder.LITTLE_ENDIAN;

    private final Object scope;
    private final long ownedMsgSlotAddress;
    private final Object msg;
    private boolean valid;
    private boolean closed;
    private boolean recvArmed;
    private boolean more;
    private int cachedSize;
    private long cachedAddress;
    static {
        ContractAccess.register(new ContractAccess.MessageAccess() {
            @Override
            public Object dataSegment(Message message) {
                return message.dataSegment();
            }

            @Override
            public Object dataSegment(Message message, int knownSize) {
                return message.dataSegment(knownSize);
            }

            @Override
            public void copyTo(Message message, Object destination) {
                message.copyTo(destination);
            }

            @Override
            public void moveTo(Message message, Object destination) {
                message.moveTo(destination);
            }

            @Override
            public Object nativeHandle(Message message) {
                return message.nativeHandle();
            }

            @Override
            public void setMore(Message message, boolean more) {
                message.setMore(more);
            }

            @Override
            public boolean more(Message message) {
                return message.more();
            }

            @Override
            public void finishReceive(Message message, boolean more) {
                message.finishReceive(more);
            }

            @Override
            public void transferTo(Message message, Object destination) {
                message.transferTo(destination);
            }

            @Override
            public void restoreFromNative(Message message, Object source,
                                          boolean moreFlag) {
                message.restoreFromNative(source, moreFlag);
            }

            @Override
            public void markTransferred(Message message) {
                message.markTransferred();
            }

            @Override
            public int moveInto(Message source, Message target,
                                boolean moreFlag) {
                return source.moveInto(target, moreFlag);
            }

            @Override
            public Message sharedCopyOf(Message message) {
                return Message.sharedFrom(message);
            }

            @Override
            public Message prepareVectorTarget(Message message) {
                return Message.prepareVectorTarget(message);
            }

            @Override
            public void finishVectorMove(Message message, boolean moreFlag) {
                message.finishVectorMove(moreFlag);
            }

            @Override
            public void resetReusable(Message message) {
                message.resetForReuse();
            }

            @Override
            public Message adoptOwnedNative(Object nativeMsg) {
                return Message.adoptOwnedNative(nativeMsg);
            }

            @Override
            public Message fromSegment(Object segment, long offset,
                                       long length) {
                return Message.from(segment, offset, length);
            }

            @Override
            public boolean isReusable(Message message) {
                return message.isReusable();
            }
        });
    }

    private Message(Object scope, boolean raw) {
        this.scope = Objects.requireNonNull(scope, "scope");
        this.ownedMsgSlotAddress = 0L;
        this.msg = ContractAccess.nativeMessageAllocate(scope);
        this.valid = false;
        this.closed = false;
        this.recvArmed = false;
        this.more = false;
        this.cachedSize = 0;
        this.cachedAddress = 0L;
    }

    private Message(Object adoptedMsg) {
        this.scope = null;
        this.ownedMsgSlotAddress = 0L;
        this.msg = Objects.requireNonNull(adoptedMsg, "adoptedMsg");
        this.valid = true;
        this.closed = false;
        this.recvArmed = false;
        this.more = false;
        cachePayload((int) ContractAccess.nativeMessageSize(adoptedMsg));
    }

    private Message(long ownedMsgSlotAddress) {
        this.scope = null;
        this.ownedMsgSlotAddress = ownedMsgSlotAddress;
        this.msg = ContractAccess.nativeMessageHandleFromAddress(
            ownedMsgSlotAddress);
        this.valid = false;
        this.closed = false;
        this.recvArmed = false;
        this.more = false;
        this.cachedSize = 0;
        this.cachedAddress = 0L;
    }

    private Message(boolean raw) {
        this(allocateOwnedMsgSlot());
    }

    public Message() {
        this(true);
        int rc = ContractAccess.nativeMessageInit(msg);
        if (rc != 0) {
            releaseOwnedResources();
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        valid = true;
        recvArmed = true;
        more = false;
        clearPayloadCache();
    }

    public Message(int size) {
        this(true);
        if (size < 0)
            throw new IllegalArgumentException("size must be >= 0");
        int rc = ContractAccess.nativeMessageInitSize(msg, size);
        if (rc != 0) {
            releaseOwnedResources();
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        valid = true;
        recvArmed = false;
        more = false;
        cachePayload(size);
    }

    public static Message allocate(int size) {
        return new Message(size);
    }

    /** Copies the full byte array into a new message-owned frame. */
    public static Message from(byte[] data) {
        return from(data, 0, data.length);
    }

    /** Copies the selected byte array range into a new message-owned frame. */
    public static Message from(byte[] data, int offset, int length) {
        Objects.requireNonNull(data, "data");
        validateRange(data.length, offset, length, "data");
        Message msg = new Message(length);
        if (length > 0) {
            UNSAFE.copyMemory(data, BYTE_ARRAY_BASE + offset, null,
                msg.cachedAddress, length);
        }
        return msg;
    }

    /** Copies the full source message into a new message-owned frame. */
    public static Message from(Message source) {
        Objects.requireNonNull(source, "source");
        int size = source.size();
        Message msg = new Message(size);
        if (size > 0) {
            UNSAFE.copyMemory(null, source.cachedAddress, null,
                msg.cachedAddress, size);
        }
        msg.more = source.more;
        return msg;
    }

    /** Moves this message into a new owned message instance. */
    Message move() {
        if (closed || !valid)
            throw new IllegalStateException("message is closed");
        Message target = new Message(true);
        boolean moreFlag = more;
        int size = cachedSize;
        transferTo(target.msg);
        target.valid = true;
        target.recvArmed = false;
        target.more = moreFlag;
        if (size > 0) {
            target.cachePayload(size);
        } else {
            target.clearPayloadCache();
        }
        return target;
    }

    /** Encodes the string as UTF-8 and copies it into a new frame. */
    public static Message from(String value) {
        Objects.requireNonNull(value, "value");
        return from(value.getBytes(StandardCharsets.UTF_8));
    }

    static Message sharedFrom(byte[] data) {
        return sharedFrom(data, 0, data.length);
    }

    static Message sharedFrom(byte[] data, int offset, int length) {
        Objects.requireNonNull(data, "data");
        validateRange(data.length, offset, length, "data");
        Message msg = new Message(ContractAccess.nativeMessageOpenSharedScope(),
            true);
        int rc = ContractAccess.nativeMessageInitSize(msg.msg, length);
        if (rc != 0) {
            ContractAccess.nativeMessageCloseScope(msg.scope);
            msg.closed = true;
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        msg.valid = true;
        msg.recvArmed = false;
        msg.more = false;
        msg.cachePayload(length);
        if (length > 0) {
            UNSAFE.copyMemory(data, BYTE_ARRAY_BASE + offset, null,
                msg.cachedAddress, length);
        }
        return msg;
    }

    static Message sharedFrom(Message source) {
        Objects.requireNonNull(source, "source");
        Message msg = new Message(ContractAccess.nativeMessageOpenSharedScope(),
            true);
        int rc = ContractAccess.nativeMessageInit(msg.msg);
        if (rc != 0) {
            ContractAccess.nativeMessageCloseScope(msg.scope);
            msg.closed = true;
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        rc = ContractAccess.nativeMessageCopy(msg.msg, source.msg);
        if (rc != 0) {
            try {
                ContractAccess.nativeMessageClose(msg.msg);
            } catch (RuntimeException ignored) {
            }
            ContractAccess.nativeMessageCloseScope(msg.scope);
            msg.closed = true;
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        msg.valid = true;
        msg.recvArmed = false;
        msg.more = source.more;
        msg.cachePayload(source.cachedSize);
        return msg;
    }

    /** Copies the remaining bytes from the buffer without mutating its cursor. */
    public static Message from(ByteBuffer data) {
        Objects.requireNonNull(data, "data");
        int length = data.remaining();
        Message msg = new Message(length);
        if (length > 0) {
            ByteBuffer src = data.slice();
            ContractAccess.nativeMessageCopyFromBuffer(src,
                msg.dataSegment(length), 0, length);
        }
        return msg;
    }

    /** Copies the readable bytes from a Netty buffer without mutating its cursor. */
    public static Message from(ByteBuf data) {
        Objects.requireNonNull(data, "data");
        int length = data.readableBytes();
        if (length <= 0) {
            return new Message(0);
        }
        int readerIndex = data.readerIndex();
        Message msg = new Message(length);
        if (data.hasMemoryAddress()) {
            UNSAFE.copyMemory(null, data.memoryAddress() + readerIndex, null,
                msg.cachedAddress, length);
            return msg;
        }
        if (data.hasArray()) {
            UNSAFE.copyMemory(data.array(),
                BYTE_ARRAY_BASE + data.arrayOffset() + readerIndex, null,
                msg.cachedAddress, length);
            return msg;
        }
        try {
            ByteBuffer source = data.nioBufferCount() == 1
                ? data.internalNioBuffer(readerIndex, length)
                : data.nioBuffer(readerIndex, length);
            ContractAccess.nativeMessageCopyFromBuffer(source,
                msg.dataSegment(length), 0, length);
            return msg;
        } catch (UnsupportedOperationException ex) {
            byte[] tmp = new byte[length];
            data.getBytes(readerIndex, tmp);
            UNSAFE.copyMemory(tmp, BYTE_ARRAY_BASE, null, msg.cachedAddress,
                length);
            return msg;
        }
    }

    /** Copies the selected memory segment range into a new message-owned frame. */
    static Message from(Object data, long offset, long length) {
        Objects.requireNonNull(data, "data");
        validateRange(Long.MAX_VALUE, offset, length, "data");
        if (length > Integer.MAX_VALUE)
            throw new IllegalArgumentException("length too large: " + length);
        Message msg = new Message((int) length);
        if (length > 0) {
            ContractAccess.nativeMessageCopyFromSegment(data, offset,
                msg.dataSegment((int) length), 0, length);
        }
        return msg;
    }

    public int size() {
        return valid && !closed ? cachedSize : 0;
    }

    public boolean more() {
        return more;
    }

    public int refCount() {
        return ContractAccess.nativeMessageRefCount(msg);
    }

    Object dataSegment() {
        return valid && !closed && cachedAddress != 0
            ? ContractAccess.nativeMessageSegmentFromAddress(cachedAddress,
                cachedSize)
            : ContractAccess.nativeMessageHandleFromAddress(0L);
    }

    Object dataSegment(int knownSize) {
        if (!valid || closed || knownSize <= 0 || cachedAddress == 0)
            return ContractAccess.nativeMessageHandleFromAddress(0L);
        return ContractAccess.nativeMessageSegmentFromAddress(cachedAddress,
            knownSize);
    }

    Object nativeHandle() {
        return msg;
    }

    public int readIntLe(int offset) {
        int size = size();
        validateRange(size, offset, Integer.BYTES, "offset");
        int value = UNSAFE.getInt(null, cachedAddress + offset);
        return NATIVE_LITTLE_ENDIAN ? value : Integer.reverseBytes(value);
    }

    public int readIntBe(int offset) {
        int size = size();
        validateRange(size, offset, Integer.BYTES, "offset");
        int value = UNSAFE.getInt(null, cachedAddress + offset);
        return NATIVE_LITTLE_ENDIAN ? Integer.reverseBytes(value) : value;
    }

    public byte readByte(int offset) {
        int size = size();
        validateRange(size, offset, 1, "offset");
        return UNSAFE.getByte(null, cachedAddress + offset);
    }

    short readShortBe(int offset) {
        int size = size();
        validateRange(size, offset, Short.BYTES, "offset");
        short value = UNSAFE.getShort(null, cachedAddress + offset);
        return NATIVE_LITTLE_ENDIAN ? Short.reverseBytes(value) : value;
    }

    public boolean contentEquals(byte[] expected) {
        Objects.requireNonNull(expected, "expected");
        int size = size();
        if (size != expected.length)
            return false;
        int i = 0;
        for (; i + Long.BYTES <= size; i += Long.BYTES) {
            long actual = UNSAFE.getLong(null, cachedAddress + i);
            long wanted = UNSAFE.getLong(expected, BYTE_ARRAY_BASE + i);
            if (actual != wanted)
                return false;
        }
        for (; i < size; i++) {
            if (UNSAFE.getByte(null, cachedAddress + i)
                != UNSAFE.getByte(expected, BYTE_ARRAY_BASE + i)) {
                return false;
            }
        }
        return true;
    }

    public long readLongLe(int offset) {
        int size = size();
        validateRange(size, offset, Long.BYTES, "offset");
        long value = UNSAFE.getLong(null, cachedAddress + offset);
        return NATIVE_LITTLE_ENDIAN ? value : Long.reverseBytes(value);
    }

    public ByteBuffer dataBuffer() {
        Object seg = dataSegment();
        if (ContractAccess.nativeMessageAddress(seg) == 0)
            return ByteBuffer.allocate(0).asReadOnlyBuffer();
        return ContractAccess.nativeMessageAsReadOnlyBuffer(seg);
    }

    public ByteBuffer mutableDataBuffer() {
        Object seg = dataSegment();
        if (ContractAccess.nativeMessageAddress(seg) == 0)
            return ByteBuffer.allocate(0);
        return ContractAccess.nativeMessageAsMutableBuffer(seg);
    }

    public byte[] toByteArray() {
        return data();
    }

    public String toUtf8String() {
        return new String(data(), StandardCharsets.UTF_8);
    }

    public boolean empty() {
        return size() == 0;
    }

    public boolean isEmpty() {
        return empty();
    }

    boolean valid() {
        return valid && !closed;
    }

    public byte[] data() {
        int size = size();
        if (size <= 0)
            return new byte[0];
        byte[] out = new byte[size];
        UNSAFE.copyMemory(null, cachedAddress, out, BYTE_ARRAY_BASE, size);
        return out;
    }

    public int copyTo(byte[] destination) {
        return copyTo(destination, 0);
    }

    public int copyTo(byte[] destination, int offset) {
        Objects.requireNonNull(destination, "destination");
        int size = size();
        validateRange(destination.length, offset, size, "destination");
        if (size == 0)
            return 0;
        UNSAFE.copyMemory(null, cachedAddress, destination,
            BYTE_ARRAY_BASE + offset, size);
        return size;
    }

    public int copyTo(byte[] destination, int sourceOffset, int destinationOffset,
                      int length) {
        Objects.requireNonNull(destination, "destination");
        int size = size();
        validateRange(size, sourceOffset, length, "source");
        validateRange(destination.length, destinationOffset, length, "destination");
        if (length == 0)
            return 0;
        UNSAFE.copyMemory(null, cachedAddress + sourceOffset, destination,
            BYTE_ARRAY_BASE + destinationOffset, length);
        return length;
    }

    public int copyTo(ByteBuffer destination) {
        Objects.requireNonNull(destination, "destination");
        int size = size();
        if (destination.remaining() < size)
            throw new IllegalArgumentException("destination buffer too small");
        if (size == 0)
            return 0;
        ByteBuffer dst = destination.slice();
        dst.limit(size);
        ContractAccess.nativeMessageCopyToBuffer(dataSegment(), 0, dst, size);
        destination.position(destination.position() + size);
        return size;
    }

    public int copyTo(ByteBuf destination) {
        Objects.requireNonNull(destination, "destination");
        int size = size();
        if (destination.writableBytes() < size)
            throw new IllegalArgumentException("destination buffer too small");
        if (size == 0)
            return 0;
        if (destination.hasMemoryAddress()) {
            int writerIndex = destination.writerIndex();
            UNSAFE.copyMemory(null, cachedAddress, null,
                destination.memoryAddress() + writerIndex, size);
            destination.writerIndex(writerIndex + size);
            return size;
        }
        destination.writeBytes(dataBuffer());
        return size;
    }

    public boolean tryCopyTo(ByteBuffer destination) {
        Objects.requireNonNull(destination, "destination");
        int size = size();
        if (destination.remaining() < size)
            return false;
        copyTo(destination);
        return true;
    }

    public boolean tryCopyTo(ByteBuf destination) {
        Objects.requireNonNull(destination, "destination");
        if (destination.writableBytes() < size())
            return false;
        copyTo(destination);
        return true;
    }

    /**
     * Reinitializes this reusable message with a new owned frame of {@code size}
     * bytes.
     *
     * <p>Sending a message transfers its native frame to the socket. Call this
     * method before filling and sending the same {@code Message} instance again.
     */
    void reset(int size) {
        if (scope == null && ownedMsgSlotAddress == 0L)
            throw new IllegalStateException("message is not reusable");
        if (size < 0)
            throw new IllegalArgumentException("size must be >= 0");
        if (closed || (scope != null
            && !ContractAccess.nativeMessageScopeAlive(scope)))
            throw new IllegalStateException("message is closed");
        if (valid) {
            int closeRc = ContractAccess.nativeMessageClose(msg);
            if (closeRc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            valid = false;
        }
        int rc = size == 0 ? ContractAccess.nativeMessageInit(msg)
            : ContractAccess.nativeMessageInitSize(msg, size);
        if (rc != 0) {
            throw ZlinkException.fromLastError(
                systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        valid = true;
        recvArmed = false;
        more = false;
        if (size == 0) {
            clearPayloadCache();
        } else {
            cachePayload(size);
        }
    }

    public void writeByte(int offset, byte value) {
        int size = size();
        validateRange(size, offset, 1, "offset");
        UNSAFE.putByte(null, cachedAddress + offset, value);
    }

    public void fill(byte value) {
        fill(value, 0, size());
    }

    public void fill(byte value, int offset, int length) {
        int size = size();
        validateRange(size, offset, length, "offset");
        if (length == 0)
            return;
        UNSAFE.setMemory(null, cachedAddress + offset, length, value);
    }

    public void writeIntLe(int offset, int value) {
        int size = size();
        validateRange(size, offset, Integer.BYTES, "offset");
        int encoded = NATIVE_LITTLE_ENDIAN ? value : Integer.reverseBytes(value);
        UNSAFE.putInt(null, cachedAddress + offset, encoded);
    }

    public void writeLongLe(int offset, long value) {
        int size = size();
        validateRange(size, offset, Long.BYTES, "offset");
        long encoded = NATIVE_LITTLE_ENDIAN ? value : Long.reverseBytes(value);
        UNSAFE.putLong(null, cachedAddress + offset, encoded);
    }

    public void writeShortBe(int offset, short value) {
        int size = size();
        validateRange(size, offset, Short.BYTES, "offset");
        short encoded = NATIVE_LITTLE_ENDIAN ? Short.reverseBytes(value) : value;
        UNSAFE.putShort(null, cachedAddress + offset, encoded);
    }

    public void writeIntBe(int offset, int value) {
        int size = size();
        validateRange(size, offset, Integer.BYTES, "offset");
        int encoded = NATIVE_LITTLE_ENDIAN ? Integer.reverseBytes(value) : value;
        UNSAFE.putInt(null, cachedAddress + offset, encoded);
    }

    public int copyFrom(byte[] source, int sourceOffset, int destinationOffset,
                        int length) {
        Objects.requireNonNull(source, "source");
        validateRange(source.length, sourceOffset, length, "source");
        int size = size();
        validateRange(size, destinationOffset, length, "destination");
        if (length == 0)
            return 0;
        UNSAFE.copyMemory(source, BYTE_ARRAY_BASE + sourceOffset, null,
            cachedAddress + destinationOffset, length);
        return length;
    }

    public int copyFrom(Message source, int sourceOffset, int destinationOffset,
                        int length) {
        Objects.requireNonNull(source, "source");
        int sourceSize = source.size();
        validateRange(sourceSize, sourceOffset, length, "source");
        int size = size();
        validateRange(size, destinationOffset, length, "destination");
        if (length == 0)
            return 0;
        UNSAFE.copyMemory(null, source.cachedAddress + sourceOffset, null,
            cachedAddress + destinationOffset, length);
        return length;
    }

    void copyTo(Object destination) {
        int rc = ContractAccess.nativeMessageInit(destination);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        rc = ContractAccess.nativeMessageCopy(destination, msg);
        if (rc != 0)
            try {
                ContractAccess.nativeMessageClose(destination);
            } catch (RuntimeException ignored) {
            }
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
    }

    void moveTo(Object destination) {
        int rc = ContractAccess.nativeMessageMove(destination, msg);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        valid = false;
        recvArmed = false;
        clearPayloadCache();
    }

    void transferTo(Object destination) {
        int rc = ContractAccess.nativeMessageInit(destination);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        rc = ContractAccess.nativeMessageMove(destination, msg);
        if (rc != 0) {
            ContractAccess.nativeMessageClose(destination);
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        }
        valid = false;
        recvArmed = false;
        more = false;
        clearPayloadCache();
    }

    void restoreFromNative(Object source, boolean moreFlag) {
        prepareForReceive();
        int rc = ContractAccess.nativeMessageMove(msg, source);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        valid = true;
        recvArmed = false;
        more = moreFlag;
        cachePayload((int) ContractAccess.nativeMessageSize(msg));
    }

    void resetForReuse() {
        if (scope == null && ownedMsgSlotAddress == 0L)
            throw new IllegalStateException("message is not reusable");
        if (closed || (scope != null
            && !ContractAccess.nativeMessageScopeAlive(scope)))
            throw new IllegalStateException("message is closed");
        if (valid) {
            int rc = ContractAccess.nativeMessageClose(msg);
            if (rc != 0)
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            valid = false;
        }
        int rc = ContractAccess.nativeMessageInit(msg);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        valid = true;
        recvArmed = true;
        more = false;
        clearPayloadCache();
    }

    boolean isReusable() {
        return !closed && (ownedMsgSlotAddress != 0
            || (scope != null && ContractAccess.nativeMessageScopeAlive(scope)));
    }

    static Message adoptOwnedNative(Object nativeMsg) {
        if (nativeMsg == null
            || ContractAccess.nativeMessageAddress(nativeMsg) == 0)
            throw new IllegalArgumentException("nativeMsg is null");
        return new Message(nativeMsg);
    }

    private static Message prepareVectorTarget(Message message) {
        Message target = message;
        if (target == null || !target.isReusable()) {
            target = new Message(true);
            int initRc = ContractAccess.nativeMessageInit(target.msg);
            if (initRc != 0) {
                target.releaseOwnedResources();
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
            }
            target.valid = true;
            target.recvArmed = true;
            target.more = false;
            target.clearPayloadCache();
        }
        return target;
    }

    private void finishVectorMove(boolean moreFlag) {
        valid = true;
        recvArmed = false;
        more = moreFlag;
        cachePayload((int) ContractAccess.nativeMessageSize(msg));
    }

    public static void closeAll(Message[] parts) {
        if (parts == null)
            return;
        for (Message part : parts) {
            if (part != null && part.isReusable()) {
                try {
                    part.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    public static void closeAll(Iterable<? extends Message> parts) {
        if (parts == null)
            return;
        for (Message part : parts) {
            if (part != null && part.isReusable()) {
                try {
                    part.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    Object handle() {
        return msg;
    }

    int moveInto(Message target, boolean moreFlag) {
        Objects.requireNonNull(target, "target");
        target.prepareForReceive();
        int rc = ContractAccess.nativeMessageMove(target.msg, msg);
        if (rc != 0)
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
        target.valid = true;
        target.recvArmed = false;
        target.more = moreFlag;
        if (cachedSize > 0) {
            target.cachePayload(cachedSize);
        } else {
            target.clearPayloadCache();
        }
        valid = false;
        recvArmed = false;
        more = false;
        clearPayloadCache();
        return target.size();
    }

    void markTransferred() {
        valid = false;
        recvArmed = false;
        more = false;
        clearPayloadCache();
    }

    void setMore(boolean moreFlag) {
        more = moreFlag;
    }

    void finishReceive(boolean moreFlag) {
        valid = true;
        recvArmed = false;
        more = moreFlag;
        cachePayload((int) ContractAccess.nativeMessageSize(msg));
    }

    @Override
    public void close() {
        if (closed)
            return;
        if (valid) {
            ContractAccess.nativeMessageClose(msg);
            valid = false;
        }
        recvArmed = false;
        more = false;
        clearPayloadCache();
        releaseOwnedResources();
    }

    private void prepareForReceive() {
        if (!recvArmed) {
            resetForReuse();
        }
    }

    private void cachePayload(int size) {
        cachedSize = size;
        cachedAddress = size > 0 ? ContractAccess.nativeMessageDataAddress(msg) : 0L;
    }

    private void clearPayloadCache() {
        cachedSize = 0;
        cachedAddress = 0L;
    }

    private void releaseOwnedResources() {
        if (scope != null && ContractAccess.nativeMessageScopeAlive(scope)) {
            ContractAccess.nativeMessageCloseScope(scope);
        } else if (ownedMsgSlotAddress != 0L) {
            MessageSlotPool.release(ownedMsgSlotAddress);
        }
        closed = true;
    }

    private static long allocateOwnedMsgSlot() {
        return MessageSlotPool.acquire();
    }

    private static void validateRange(int total, int offset, int length, String name) {
        if (offset < 0 || length < 0 || offset > total - length)
            throw new IndexOutOfBoundsException(name + " range out of bounds");
    }

    private static void validateRange(long total, long offset, long length, String name) {
        if (offset < 0 || length < 0 || offset > total - length)
            throw new IndexOutOfBoundsException(name + " range out of bounds");
    }

    private static final class UnsafeAccess {
        private static final Unsafe INSTANCE = lookupUnsafe();

        private UnsafeAccess() {
        }

        static Unsafe get() {
            return INSTANCE;
        }

        private static Unsafe lookupUnsafe() {
            try {
                Field field = Unsafe.class.getDeclaredField("theUnsafe");
                field.setAccessible(true);
                return (Unsafe) field.get(null);
            } catch (ReflectiveOperationException ex) {
                throw new IllegalStateException(
                    "Unable to access sun.misc.Unsafe", ex);
            }
        }
    }

    private static final class MessageSlotPool {
        private static final long MESSAGE_LAYOUT_SIZE =
            ContractAccess.nativeMessageLayoutSize();
        private static final ThreadLocal<Pool> POOL =
            ThreadLocal.withInitial(Pool::new);

        private MessageSlotPool() {
        }

        static long acquire() {
            return POOL.get().acquire();
        }

        static void release(long slot) {
            POOL.get().release(slot);
        }

        private static final class Pool {
            private static final int CAPACITY = 32;
            private final long[] slots = new long[CAPACITY];
            private int count;

            long acquire() {
                if (count > 0) {
                    count--;
                    long slot = slots[count];
                    slots[count] = 0L;
                    return slot;
                }
                long address = UNSAFE.allocateMemory(MESSAGE_LAYOUT_SIZE);
                UNSAFE.setMemory(address, MESSAGE_LAYOUT_SIZE, (byte) 0);
                return address;
            }

            void release(long slot) {
                if (count < CAPACITY) {
                    slots[count++] = slot;
                } else {
                    UNSAFE.freeMemory(slot);
                }
            }
        }
    }

}
