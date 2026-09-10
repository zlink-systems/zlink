/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

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
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.RecvScratch;

final class ReceivePlane {
    private static final ContractAccess.ReceivedAccess RECEIVED_ACCESS =
        ContractAccess.receivedAccessForRuntime();
    private final NativeSocketRuntime socket;
    private final ThreadLocal<MultipartReceiveState> multipartReceiveState =
        ThreadLocal.withInitial(MultipartReceiveState::new);
    private final ThreadLocal<Received> activeLazyReceive = new ThreadLocal<>();

    ReceivePlane(NativeSocketRuntime socket) {
        this.socket = socket;
    }

    boolean recvInto(Received result, ReceiveFlag flags) {
        Objects.requireNonNull(result, "result");
        Objects.requireNonNull(flags, "flags");
        prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        Message[] parts = receivePartsOrNull(scratch, flags,
            flags == ReceiveFlag.DONTWAIT);
        if (parts == null) {
            return false;
        }
        boolean adopted = false;
        try {
            populate(result, parts, null, 0L, false);
            adopted = true;
            return true;
        } finally {
            if (!adopted) {
                Message.closeAll(parts);
            }
        }
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
        if (length == 0) {
            return 0;
        }
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
        if (length == 0) {
            return 0;
        }
        try (Message frame = nextRecvFrame(flags, true)) {
            if (frame == null) {
                return -1;
            }
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
        if (frame == null) {
            return -1;
        }
        try {
            return InternalAccess.messageMoveInto(frame, message,
                frame.more());
        } finally {
            frame.close();
        }
    }

    Message nextRecvFrame(ReceiveFlag flags, boolean nonBlocking) {
        Objects.requireNonNull(flags, "flags");
        MultipartReceiveState state = multipartReceiveState.get();
        if (state.hasPending()) {
            return state.poll();
        }
        prepareRecvLikeOperation(state);
        RecvScratch scratch = socket.recvScratch();
        Message[] parts = receivePartsOrNull(scratch, flags, nonBlocking);
        if (parts == null) {
            return null;
        }
        Message routingFrame = NativeRoutingIds.readRoutingFrameOut(
            scratch.sourceRidOut);
        if (routingFrame != null) {
            state.replace(parts);
            InternalAccess.messageSetMore(routingFrame, true);
            return routingFrame;
        }
        if (parts.length == 1) {
            return parts[0];
        }
        state.replace(parts);
        return state.poll();
    }

    void prepareRecvLikeOperation() {
        prepareRecvLikeOperation(multipartReceiveState.get());
    }

    private void prepareRecvLikeOperation(MultipartReceiveState state) {
        state.closeRemaining();
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

    private Received recvLazyOrNull(ReceiveFlag flags, boolean allowNoData) {
        Objects.requireNonNull(flags, "flags");
        prepareRecvLikeOperation();
        RecvScratch scratch = socket.recvScratch();
        Message[] parts = receivePartsOrNull(scratch, flags, allowNoData);
        if (parts == null) {
            return null;
        }
        boolean adopted = false;
        try {
            RoutingId routingId = NativeRoutingIds.readOut(
                scratch.sourceRidOut);
            Received received = InternalAccess.received(routingId, parts,
                true, 0L, false, null, null);
            adopted = true;
            return received;
        } finally {
            if (!adopted) {
                Message.closeAll(parts);
            }
        }
    }

    private Message[] receivePartsOrNull(RecvScratch scratch,
                                         ReceiveFlag flags,
                                         boolean allowNoData) {
        while (true) {
            int rc = flags == ReceiveFlag.DONTWAIT
                ? Native.recvNoWaitCritical(socket.handle(),
                    scratch.sourceRidOut, scratch.partsOut,
                    scratch.partCountOut, flags.getValue())
                : Native.recv(socket.handle(), scratch.sourceRidOut,
                    scratch.partsOut, scratch.partCountOut, flags.getValue());
            if (rc == RecvResult.OK.value()) {
                return InternalAccess.messageFromOwnedMessageVector(
                    scratch.partsOut.get(ValueLayout.ADDRESS, 0),
                    scratch.partCountOut.get(ValueLayout.JAVA_LONG, 0));
            }
            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                continue;
            }
            RecvResult result = RecvResult.fromValue(rc);
            if (allowNoData && (result == RecvResult.NO_DATA
                || result == RecvResult.BUSY)) {
                return null;
            }
            throw new ZlinkRecvException(result, errno);
        }
    }

    private static void populate(Received target, Message[] parts,
                                 byte[] routingIdBytes,
                                 long replyTokenValue,
                                 boolean hasReplyToken) {
        if (parts.length == 1) {
            RECEIVED_ACCESS.populateRoutedSinglePart(target, routingIdBytes,
                parts[0], replyTokenValue, hasReplyToken, null, null);
        } else if (parts.length == 2) {
            RECEIVED_ACCESS.populateRoutedTwoParts(target, routingIdBytes,
                parts[0], parts[1], replyTokenValue, hasReplyToken, null,
                null);
        } else {
            RECEIVED_ACCESS.populateRoutedParts(target, routingIdBytes, parts,
                replyTokenValue, hasReplyToken, null, null);
        }
    }
}
