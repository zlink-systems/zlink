/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.messaging;

import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMessage;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;

/**
 * Native-backed message operations used by the public Message value.
 */
final class NativeMessageRuntime {
    static {
        ContractAccess.register(new ContractAccess.NativeMessageAccess() {
            @Override
            public long layoutSize() {
                return NativeMessageRuntime.layoutSize();
            }

            @Override
            public Object openSharedMessageScope() {
                return Arena.ofShared();
            }

            @Override
            public boolean messageScopeAlive(Object scope) {
                return ((Arena) scope).scope().isAlive();
            }

            @Override
            public void closeMessageScope(Object scope) {
                ((Arena) scope).close();
            }

            @Override
            public Object allocate(Object arena) {
                return NativeMessageRuntime.allocate((Arena) arena);
            }

            @Override
            public Object handleFromAddress(long address) {
                return MemorySegment.ofAddress(address)
                    .reinterpret(NativeMessageRuntime.layoutSize());
            }

            @Override
            public Object segmentFromAddress(long address, long size) {
                return MemorySegment.ofAddress(address).reinterpret(size);
            }

            @Override
            public Object slice(Object segment, long offset, long size) {
                return ((MemorySegment) segment).asSlice(offset, size);
            }

            @Override
            public long address(Object segment) {
                return ((MemorySegment) segment).address();
            }

            @Override
            public int init(Object msg) {
                return NativeMessageRuntime.init((MemorySegment) msg);
            }

            @Override
            public int initSize(Object msg, int size) {
                return NativeMessageRuntime.initSize((MemorySegment) msg,
                    size);
            }

            @Override
            public int close(Object msg) {
                return NativeMessageRuntime.close((MemorySegment) msg);
            }

            @Override
            public int move(Object destination, Object source) {
                return NativeMessageRuntime.move((MemorySegment) destination,
                    (MemorySegment) source);
            }

            @Override
            public int copy(Object destination, Object source) {
                return NativeMessageRuntime.copy((MemorySegment) destination,
                    (MemorySegment) source);
            }

            @Override
            public long size(Object msg) {
                return NativeMessageRuntime.size((MemorySegment) msg);
            }

            @Override
            public long dataAddress(Object msg) {
                return NativeMessageRuntime.dataAddress((MemorySegment) msg);
            }

            @Override
            public int refCount(Object msg) {
                return NativeMessageRuntime.refCount((MemorySegment) msg);
            }

            @Override
            public void closeVector(Object parts, long count) {
                NativeMessageRuntime.closeVector((MemorySegment) parts,
                    count);
            }

            @Override
            public Message[] materializeVector(Object parts, long count,
                                               Message[] reusable,
                                               boolean closeSourceVector) {
                return NativeMessageRuntime.materializeVector(
                    (MemorySegment) parts, count, reusable, closeSourceVector);
            }

            @Override
            public Message[] materializeVectorShared(Object parts, long count) {
                return NativeMessageRuntime.materializeVectorShared(
                    (MemorySegment) parts, count);
            }

            @Override
            public Message adoptOwnedMessage(Object nativeMsg) {
                return ContractAccess.messageAdoptOwnedNative(nativeMsg);
            }

            @Override
            public void copyFromSegment(Object source, long sourceOffset,
                                        Object destination,
                                        long destinationOffset, long length) {
                MemorySegment.copy((MemorySegment) source, sourceOffset,
                    (MemorySegment) destination, destinationOffset, length);
            }

            @Override
            public void copyFromBuffer(ByteBuffer source, Object destination,
                                       long destinationOffset, long length) {
                MemorySegment.copy(MemorySegment.ofBuffer(source), 0,
                    (MemorySegment) destination, destinationOffset, length);
            }

            @Override
            public void copyToBuffer(Object source, long sourceOffset,
                                     ByteBuffer destination, long length) {
                MemorySegment.copy((MemorySegment) source, sourceOffset,
                    MemorySegment.ofBuffer(destination), 0, length);
            }

            @Override
            public ByteBuffer asReadOnlyBuffer(Object source) {
                return ((MemorySegment) source).asByteBuffer().asReadOnlyBuffer();
            }

            @Override
            public ByteBuffer asMutableBuffer(Object source) {
                return ((MemorySegment) source).asByteBuffer();
            }
        });
    }

    private NativeMessageRuntime() {
    }

    static long layoutSize() {
        return NativeLayouts.MESSAGE_LAYOUT.byteSize();
    }

    static MemorySegment allocate(Arena arena) {
        return arena.allocate(NativeLayouts.MESSAGE_LAYOUT);
    }

    static int init(MemorySegment msg) {
        return NativeMessage.messageInit(msg);
    }

    static int initSize(MemorySegment msg, int size) {
        return NativeMessage.messageInitSize(msg, size);
    }

    static int close(MemorySegment msg) {
        return NativeMessage.messageClose(msg);
    }

    static int move(MemorySegment destination, MemorySegment source) {
        return NativeMessage.messageMove(destination, source);
    }

    static int copy(MemorySegment destination, MemorySegment source) {
        return NativeMessage.messageCopy(destination, source);
    }

    static long size(MemorySegment msg) {
        return NativeMessage.messageSize(msg);
    }

    static long dataAddress(MemorySegment msg) {
        return NativeMessage.messageDataAddress(msg);
    }

    static int refCount(MemorySegment msg) {
        return NativeMessage.messageRefCount(msg);
    }

    static void closeVector(MemorySegment parts, long count) {
        NativeMessage.messageVectorClose(parts, count);
    }

    static Message[] materializeVector(MemorySegment partsAddr,
                                       long count,
                                       Message[] reusable,
                                       boolean closeSourceVector) {
        if (partsAddr == null || partsAddr.address() == 0 || count <= 0)
            return new Message[0];
        if (count > Integer.MAX_VALUE)
            throw new IllegalArgumentException("msg vector too large: " + count);
        long messageSize = layoutSize();
        if (count > Long.MAX_VALUE / messageSize)
            throw new IllegalArgumentException("msg vector too large: " + count);

        int outSize = (int) count;
        Message[] out;
        if (reusable == null || reusable.length != outSize) {
            out = new Message[outSize];
            if (reusable != null) {
                System.arraycopy(reusable, 0, out, 0, Math.min(reusable.length,
                    out.length));
            }
        } else {
            out = reusable;
        }

        int built = 0;
        boolean success = false;
        MemorySegment parts = MemorySegment.ofAddress(partsAddr.address())
            .reinterpret(messageSize * count);
        try {
            for (int i = 0; i < count; i++) {
                MemorySegment src = parts.asSlice((long) i * messageSize, messageSize);
                Message msg = ContractAccess.messagePrepareVectorTarget(out[i]);
                out[i] = msg;
                int rc = move((MemorySegment) ContractAccess.messageNativeHandle(msg),
                    src);
                if (rc != 0)
                    throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.CONFIG);
                ContractAccess.messageFinishVectorMove(msg, i + 1 < count);
                built++;
            }
            success = true;
            return out;
        } finally {
            if (closeSourceVector) {
                closeVector(partsAddr, count);
                if (!success) {
                    resetBuilt(out, built);
                }
            } else if (!success) {
                closeRemaining(parts, built, count, messageSize);
                Message.closeAll(out);
            }
        }
    }

    static Message[] materializeVectorShared(MemorySegment partsAddr,
                                             long count) {
        return materializeVector(partsAddr, count, null, false);
    }

    private static void resetBuilt(Message[] out, int built) {
        for (int i = 0; i < built; i++) {
            if (out[i] != null && ContractAccess.messageIsReusable(out[i])) {
                try {
                    ContractAccess.messageResetReusable(out[i]);
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    private static void closeRemaining(MemorySegment parts, int built,
                                       long count, long messageSize) {
        for (int i = built; i < count; i++) {
            MemorySegment src = parts.asSlice((long) i * messageSize, messageSize);
            try {
                close(src);
            } catch (RuntimeException ignored) {
            }
        }
    }
}
