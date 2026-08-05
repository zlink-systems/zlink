/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.runtime.nativeapi.InternalAccess;
import io.netty.buffer.ByteBuf;
import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.util.Objects;

final class NettySocketPlane {
    private static final byte[] EMPTY_BYTES = new byte[0];

    private final NativeSocketRuntime runtime;
    private final ThreadLocal<byte[]> sendScratch =
        ThreadLocal.withInitial(() ->
            new byte[NativeSocketRuntime.DEFAULT_IO_BUFFER_SIZE]);

    NettySocketPlane(NativeSocketRuntime runtime) {
        this.runtime = runtime;
    }

    int send(ByteBuf buf, int sendFlags) {
        Objects.requireNonNull(buf, "buf");
        int len = buf.readableBytes();
        if (len <= 0) {
            try (Message msg = Message.from(EMPTY_BYTES)) {
                runtime.sendMessageFrame(msg, SendFlag.fromValue(sendFlags));
                return 0;
            }
        }

        int readerIndex = buf.readerIndex();
        MemorySegment directSeg = segmentAt(buf, readerIndex, len);
        if (directSeg.address() != 0) {
            int rc = runtime.send(directSeg, 0, len, sendFlags);
            if (rc > 0) {
                buf.readerIndex(readerIndex + rc);
            }
            return rc;
        }
        return sendFallback(buf, readerIndex, len, sendFlags);
    }

    boolean sendNoWaitResult(ByteBuf buf, int sendFlags) {
        Objects.requireNonNull(buf, "buf");
        int len = buf.readableBytes();
        if (len <= 0) {
            try (Message msg = Message.from(EMPTY_BYTES)) {
                return runtime.sendMessageFrameNoWaitResult(msg,
                    SendFlag.fromValue(sendFlags));
            }
        }

        int readerIndex = buf.readerIndex();
        MemorySegment directSeg = segmentAt(buf, readerIndex, len);
        if (directSeg.address() != 0) {
            boolean sent = runtime.sendNoWaitResult(directSeg, 0, len,
                sendFlags);
            if (sent) {
                buf.readerIndex(readerIndex + len);
            }
            return sent;
        }
        return sendFallbackNoWait(buf, readerIndex, len, sendFlags);
    }

    int recvByteBufDirect(ByteBuf buf, ReceiveFlag flags) {
        int writable = buf.writableBytes();
        if (writable <= 0) {
            return 0;
        }

        int writerIndex = buf.writerIndex();
        MemorySegment directSeg = segmentAt(buf, writerIndex, writable);
        if (directSeg.address() != 0) {
            int rc = runtime.recv(directSeg, 0, writable, flags);
            if (rc > 0) {
                buf.writerIndex(writerIndex + rc);
            }
            return rc;
        }
        return recvFallback(buf, writerIndex, writable, flags);
    }

    int recvByteBufDirectNoWait(ByteBuf buf, ReceiveFlag flags) {
        int writable = buf.writableBytes();
        if (writable <= 0) {
            return 0;
        }

        int writerIndex = buf.writerIndex();
        MemorySegment directSeg = segmentAt(buf, writerIndex, writable);
        if (directSeg.address() != 0) {
            int rc = runtime.recvNoWait(directSeg, 0, writable, flags);
            if (rc > 0) {
                buf.writerIndex(writerIndex + rc);
            }
            return rc;
        }
        return recvFallbackNoWait(buf, writerIndex, writable, flags);
    }

    private static MemorySegment segmentAt(ByteBuf buf, int index,
                                           int length) {
        if (length <= 0 || !buf.hasMemoryAddress()) {
            return MemorySegment.NULL;
        }
        return MemorySegment.ofAddress(buf.memoryAddress() + index)
            .reinterpret(length);
    }

    private int sendFallback(ByteBuf buf, int readerIndex, int length,
                             int sendFlags) {
        int rc;
        if (buf.hasArray()) {
            rc = runtime.send(buf.array(), buf.arrayOffset() + readerIndex,
                length, sendFlags);
        } else {
            byte[] tmp = sendArray(length);
            buf.getBytes(readerIndex, tmp, 0, length);
            rc = runtime.send(tmp, 0, length, sendFlags);
        }
        if (rc > 0) {
            buf.readerIndex(readerIndex + rc);
        }
        return rc;
    }

    private boolean sendFallbackNoWait(ByteBuf buf, int readerIndex, int length,
                                       int sendFlags) {
        boolean sent;
        if (buf.hasArray()) {
            sent = runtime.sendNoWaitResult(buf.array(),
                buf.arrayOffset() + readerIndex, length, sendFlags);
        } else {
            byte[] tmp = sendArray(length);
            buf.getBytes(readerIndex, tmp, 0, length);
            sent = runtime.sendNoWaitResult(tmp, 0, length, sendFlags);
        }
        if (sent) {
            buf.readerIndex(readerIndex + length);
        }
        return sent;
    }

    private byte[] sendArray(int length) {
        if (length <= NativeSocketRuntime.DEFAULT_IO_BUFFER_SIZE) {
            return sendScratch.get();
        }
        return new byte[length];
    }

    private int recvFallback(ByteBuf buf, int writerIndex, int writable,
                             ReceiveFlag flags) {
        try (Message frame = runtime.nextRecvFrame(flags, false)) {
            return copyFrame(frame, buf, writerIndex, writable);
        }
    }

    private int recvFallbackNoWait(ByteBuf buf, int writerIndex, int writable,
                                   ReceiveFlag flags) {
        try (Message frame = runtime.nextRecvFrame(flags, true)) {
            if (frame == null) {
                return -1;
            }
            return copyFrame(frame, buf, writerIndex, writable);
        }
    }

    private static int copyFrame(Message frame, ByteBuf buf, int writerIndex,
                                 int writable) {
        int rc = Math.min(writable, frame.size());
        if (rc <= 0) {
            return rc;
        }
        if (buf.hasArray()) {
            MemorySegment.copy(InternalAccess.messageDataSegment(frame, rc), 0,
                MemorySegment.ofArray(buf.array()),
                (long) buf.arrayOffset() + writerIndex, rc);
        } else {
            ByteBuffer src = InternalAccess.messageDataSegment(frame)
                .asSlice(0, rc)
                .asByteBuffer();
            buf.setBytes(writerIndex, src);
        }
        buf.writerIndex(writerIndex + rc);
        return rc;
    }

}
