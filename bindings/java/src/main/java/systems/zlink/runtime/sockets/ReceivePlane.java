/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.messaging.ReceivedPartCursor;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.RecvScratch;

final class ReceivePlane {
    private final NativeSocketRuntime socket;
    private final ThreadLocal<MultipartReceiveState> multipartReceiveState =
        ThreadLocal.withInitial(MultipartReceiveState::new);
    private final ThreadLocal<Received> activeLazyReceive =
        new ThreadLocal<>();

    ReceivePlane(NativeSocketRuntime socket) {
        this.socket = socket;
    }

    boolean recvInto(Received result, ReceiveFlag flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        if (flags == ReceiveFlag.DONTWAIT
            && !multipartReceiveState.get().hasPending()) {
            return recvIntoNoWait(result);
        }
        Message frame = nextRecvFrame(flags, flags == ReceiveFlag.DONTWAIT);
        if (frame == null) {
            return false;
        }
        if (!frame.more()) {
            ContractAccess.receivedPopulateRoutedSinglePart(result, null, frame,
                0L, false, null, null);
            return true;
        }

        Received fresh = InternalAccess.receivedLazy((byte[]) null, frame,
            new BasicReceiveCursor(flags.getValue()), 0L, false, null, null);
        ContractAccess.receivedAdoptFrom(result, fresh);
        return true;
    }

    Received recvLazy(ReceiveFlag flags) {
        Received received = recvLazyOrNull(flags, false);
        if (received == null) {
            throw new ZlinkRecvException(RecvResult.NO_DATA,
                NativeErrno.EAGAIN);
        }
        return received;
    }

    Received recvLazyNoWaitOrNull() {
        return recvLazyOrNull(ReceiveFlag.DONTWAIT, true);
    }

    int recv(MemorySegment segment, long offset, long length,
             ReceiveFlag flags) {
        Objects.requireNonNull(segment, "segment");
        NativeSocketRuntime.validateRange(segment.byteSize(), offset, length,
            "segment");
        if (length == 0)
            return 0;
        try (Message frame = nextRecvFrame(flags, false)) {
            int rc = Math.min(NativeSocketRuntime.toIntLength(length),
                frame.size());
            if (rc > 0) {
                MemorySegment.copy(InternalAccess.messageDataSegment(frame), 0,
                    segment, offset, rc);
            }
            return rc;
        }
    }

    int recvNoWait(MemorySegment segment, long offset, long length,
                   ReceiveFlag flags) {
        Objects.requireNonNull(segment, "segment");
        NativeSocketRuntime.validateRange(segment.byteSize(), offset, length,
            "segment");
        if (length == 0)
            return 0;
        try (Message frame = nextRecvFrame(flags, true)) {
            if (frame == null)
                return -1;
            int rc = Math.min(NativeSocketRuntime.toIntLength(length),
                frame.size());
            if (rc > 0) {
                MemorySegment.copy(InternalAccess.messageDataSegment(frame), 0,
                    segment, offset, rc);
            }
            return rc;
        }
    }

    void recvMessageFrame(Message message, ReceiveFlag flag) {
        Message frame = nextRecvFrame(flag, false);
        try {
            InternalAccess.messageMoveInto(frame, message, frame.more());
        } finally {
            frame.close();
        }
    }

    int recvMessageFrameNoWait(Message message, ReceiveFlag flag) {
        Message frame = nextRecvFrame(flag, true);
        if (frame == null)
            return -1;
        try {
            return InternalAccess.messageMoveInto(frame, message, frame.more());
        } finally {
            frame.close();
        }
    }

    Message nextRecvFrame(ReceiveFlag flags, boolean nonBlocking) {
        Objects.requireNonNull(flags, "flags");
        prepareRecvLikeOperation();
        MultipartReceiveState state = multipartReceiveState.get();
        if (state.hasPending()) {
            return state.poll();
        }
        RecvScratch scratch = socket.recvScratch();
        Message firstPart = recvSocketPartOrNull(scratch, flags, nonBlocking);
        if (firstPart == null) {
            return null;
        }
        boolean hasMore = firstPart.more();
        Message routingFrame = NativeRoutingIds.readRoutingFrameOut(
            scratch.sourceRidOut);
        if (routingFrame == null) {
            return firstPart;
        }
        if (hasMore) {
            InternalAccess.messageSetMore(firstPart, true);
        }
        state.replace(new Message[] {firstPart});
        InternalAccess.messageSetMore(routingFrame, true);
        return routingFrame;
    }

    void prepareRecvLikeOperation() {
        multipartReceiveState.get().closeRemaining();
        Received active = activeLazyReceive.get();
        if (active != null) {
            InternalAccess.receivedForceMaterialize(active);
            activeLazyReceive.remove();
        }
    }

    Runnable lazyReceiveCompletion(Received received) {
        return () -> {
            Received active = activeLazyReceive.get();
            if (active == received) {
                activeLazyReceive.remove();
            }
        };
    }

    Received registerLazyReceive(Received received, boolean hasMore) {
        if (hasMore) {
            activeLazyReceive.set(received);
        }
        return received;
    }

    int pendingFrameCount() {
        return multipartReceiveState.get().pendingCount();
    }

    private boolean recvIntoNoWait(Received result) {
        prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        while (true) {
            Message frame = new Message();
            boolean success = false;
            try {
                int rc = Native.recvPartNoWaitCritical(socket.handle(),
                    scratch.sourceRidOut,
                    InternalAccess.messageNativeHandle(frame),
                    scratch.hasMoreOut, ReceiveFlag.DONTWAIT.getValue());
                if (rc == 0) {
                    success = true;
                    boolean hasMore =
                        scratch.hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                    InternalAccess.messageFinishReceive(frame, hasMore);
                    if (!hasMore) {
                        ContractAccess.receivedPopulateRoutedSinglePart(result,
                            null, frame, 0L, false, null, null);
                    } else {
                        Received fresh = InternalAccess.receivedLazy(
                            (byte[]) null, frame,
                            new BasicReceiveCursor(
                                ReceiveFlag.DONTWAIT.getValue()),
                            0L, false, null, null);
                        ContractAccess.receivedAdoptFrom(result, fresh);
                    }
                    return true;
                }
            } finally {
                if (!success) {
                    try {
                        frame.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }

            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                continue;
            }
            if (errno == NativeErrno.EAGAIN
                || errno == NativeErrno.EWOULDBLOCK_WIN) {
                return false;
            }
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.RECV);
        }
    }

    private Received recvLazyOrNull(ReceiveFlag flags, boolean allowNoData) {
        Objects.requireNonNull(flags, "flags");
        prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        Message firstPart = recvSocketPartOrNull(scratch, flags, allowNoData);
        if (firstPart == null) {
            return null;
        }
        boolean hasMore = firstPart.more();
        RoutingId routingId = NativeRoutingIds.readOut(
            scratch.sourceRidOut);
        ReceivedPartCursor cursor = hasMore
            ? new BasicReceiveCursor(flags.getValue())
            : null;
        Received[] ref = new Received[1];
        Runnable onTerminal = () -> {
            Received active = activeLazyReceive.get();
            if (active == ref[0]) {
                activeLazyReceive.remove();
            }
        };
        Received received = InternalAccess.receivedLazy(
            routingId, firstPart, cursor, 0L, false,
            null, onTerminal);
        ref[0] = received;
        return registerLazyReceive(received, hasMore);
    }

    private Message recvSocketPartOrNull(RecvScratch scratch,
                                         ReceiveFlag flags,
                                         boolean allowNoData) {
        while (true) {
            Message part = new Message();
            boolean success = false;
            try {
                int rc = Native.recv(socket.handle(), scratch.sourceRidOut,
                    InternalAccess.messageNativeHandle(part),
                    scratch.hasMoreOut, flags.getValue());
                if (rc == 0) {
                    success = true;
                    boolean hasMore =
                        scratch.hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                    InternalAccess.messageFinishReceive(part, hasMore);
                    return part;
                }
            } finally {
                if (!success) {
                    try {
                        part.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }

            int errno = Native.errno();
            if (errno == NativeErrno.EINTR)
                continue;
            if (allowNoData
                && (errno == NativeErrno.EAGAIN
                    || errno == NativeErrno.EWOULDBLOCK_WIN)) {
                return null;
            }
            throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.RECV);
        }
    }

    private final class BasicReceiveCursor implements ReceivedPartCursor {
        private final int flags;
        private final Arena arena = Arena.ofConfined();
        private final MemorySegment sourceRidOut = arena.allocate(
            ValueLayout.ADDRESS);
        private final MemorySegment hasMoreOut = arena.allocate(
            ValueLayout.JAVA_INT);
        private boolean hasMore = true;
        private boolean closed;

        private BasicReceiveCursor(int flags) {
            this.flags = flags;
        }

        @Override
        public Message nextPartOrNull() {
            if (closed || !hasMore)
                return null;
            while (true) {
                Message next = new Message();
                boolean success = false;
                try {
                    int rc = Native.recv(socket.handle(), sourceRidOut,
                        InternalAccess.messageNativeHandle(next), hasMoreOut,
                        flags);
                    if (rc == 0) {
                        success = true;
                        hasMore = hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                        InternalAccess.messageFinishReceive(next, hasMore);
                        if (!hasMore) {
                            closeArena();
                        }
                        return next;
                    }
                } finally {
                    if (!success) {
                        try {
                            next.close();
                        } catch (RuntimeException ignored) {
                        }
                    }
                }

                int errno = Native.errno();
                if (errno == NativeErrno.EINTR)
                    continue;
                closeArena();
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.RECV);
            }
        }

        @Override
        public void close() {
            if (closed)
                return;
            while (hasMore) {
                Message next = nextPartOrNull();
                if (next == null)
                    break;
                try {
                    next.close();
                } catch (RuntimeException ignored) {
                }
            }
            closed = true;
            closeArena();
        }

        private void closeArena() {
            hasMore = false;
            if (arena.scope().isAlive()) {
                arena.close();
            }
        }
    }
}
