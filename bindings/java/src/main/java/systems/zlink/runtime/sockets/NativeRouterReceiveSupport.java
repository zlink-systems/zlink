/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.function.BiConsumer;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.internal.ContractAccess;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;

final class NativeRouterReceiveSupport implements AutoCloseable {
    private final NativeRouterSocket socket;
    private final boolean closeSocketOnClose;
    private volatile boolean closed;
    private static final ThreadLocal<RecvOutScratch> RECV_OUT_SCRATCH =
        ThreadLocal.withInitial(RecvOutScratch::new);

    static final class RecvOutScratch {
        final MemorySegment sourceNodeRidOut;
        final MemorySegment replyTokenValueOut;
        final MemorySegment partsOut;
        final MemorySegment partCountOut;

        RecvOutScratch() {
            Arena auto = Arena.ofAuto();
            sourceNodeRidOut = auto.allocate(ValueLayout.ADDRESS);
            replyTokenValueOut = auto.allocate(ValueLayout.JAVA_LONG);
            partsOut = auto.allocate(ValueLayout.ADDRESS);
            partCountOut = auto.allocate(ValueLayout.JAVA_LONG);
        }
    }

    private record NativeRecord(byte[] routingIdBytes, Message[] parts,
                                long replyTokenValue) {
    }

    NativeRouterReceiveSupport(RouterSocket socket) {
        this(socket, true);
    }

    NativeRouterReceiveSupport(RouterSocket socket,
                               boolean closeSocketOnClose) {
        this.socket = (NativeRouterSocket) Objects.requireNonNull(socket,
            "socket");
        this.closeSocketOnClose = closeSocketOnClose;
    }

    public RouterSocket socket() {
        return socket;
    }

    public Received recv() {
        return recv(RecvFlags.NONE);
    }

    public Received recv(RecvFlags flags) {
        Objects.requireNonNull(flags, "flags");
        NativeRecord record = receive(flags, false);
        return toReceived(record);
    }

    Received recvNoWaitOrNull() {
        NativeRecord record = receive(RecvFlags.DONT_WAIT, true);
        return record == null ? null : toReceived(record);
    }

    public boolean recvInto(Received target, RecvFlags flags) {
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(flags, "flags");
        NativeRecord record = receive(flags, flags == RecvFlags.DONT_WAIT);
        if (record == null) {
            return false;
        }
        boolean adopted = false;
        try {
            Message[] parts = record.parts();
            BiConsumer<List<Message>, SendFlags> sender = replySender(record);
            if (parts.length == 1) {
                ContractAccess.receivedPopulateRoutedSinglePart(target,
                    record.routingIdBytes(), parts[0],
                    record.replyTokenValue(), record.replyTokenValue() != 0L,
                    sender, null);
            } else if (parts.length == 2) {
                ContractAccess.receivedPopulateRoutedTwoParts(target,
                    record.routingIdBytes(), parts[0], parts[1],
                    record.replyTokenValue(), record.replyTokenValue() != 0L,
                    sender, null);
            } else {
                ContractAccess.receivedPopulateRoutedParts(target,
                    record.routingIdBytes(), parts, record.replyTokenValue(),
                    record.replyTokenValue() != 0L, sender, null);
            }
            adopted = true;
            return true;
        } finally {
            if (!adopted) {
                Message.closeAll(record.parts());
            }
        }
    }

    Optional<Received> recvNoWait() {
        return Optional.ofNullable(recvNoWaitOrNull());
    }

    @Override
    public void close() {
        beginClose();
        try {
            if (closeSocketOnClose) {
                socket.close();
            }
        } finally {
            finishClose();
        }
    }

    public void beginClose() {
        if (!closed) {
            closed = true;
        }
    }

    public void finishClose() {
    }

    private NativeRecord receive(RecvFlags flags, boolean nullOnNoData) {
        RecvOutScratch scratch = RECV_OUT_SCRATCH.get();
        while (true) {
            int rc = flags == RecvFlags.DONT_WAIT
                ? Native.routerRecvNoWaitCritical(
                    InternalAccess.socketHandle(socket),
                    scratch.sourceNodeRidOut, scratch.replyTokenValueOut,
                    scratch.partsOut, scratch.partCountOut, flags.value())
                : Native.routerRecv(InternalAccess.socketHandle(socket),
                    scratch.sourceNodeRidOut, scratch.replyTokenValueOut,
                    scratch.partsOut, scratch.partCountOut, flags.value());
            if (rc == RecvResult.OK.value()) {
                Message[] parts =
                    InternalAccess.messageFromOwnedMessageVector(
                        scratch.partsOut.get(ValueLayout.ADDRESS, 0),
                        scratch.partCountOut.get(ValueLayout.JAVA_LONG, 0));
                return new NativeRecord(
                    NativeRoutingIds.readBytesOut(scratch.sourceNodeRidOut),
                    parts, scratch.replyTokenValueOut.get(
                        ValueLayout.JAVA_LONG, 0));
            }
            int errno = Native.errno();
            if (errno == NativeErrno.EINTR) {
                continue;
            }
            RecvResult result = RecvResult.fromValue(rc);
            if (nullOnNoData && (result == RecvResult.NO_DATA
                || result == RecvResult.BUSY)) {
                return null;
            }
            throw new ZlinkRecvException(result, errno);
        }
    }

    private Received toReceived(NativeRecord record) {
        boolean adopted = false;
        try {
            long token = record.replyTokenValue();
            Received received;
            if (token == 0L) {
                received = InternalAccess.received(record.routingIdBytes(),
                    record.parts(), true, 0L, false, null, null);
            } else {
                RoutingId rid = ContractAccess.routingIdFromTrusted(
                    record.routingIdBytes());
                received = InternalAccess.received(rid, record.parts(), true,
                    token, true, replySender(rid, token), null);
            }
            adopted = true;
            return received;
        } finally {
            if (!adopted) {
                Message.closeAll(record.parts());
            }
        }
    }

    private BiConsumer<List<Message>, SendFlags> replySender(
            NativeRecord record) {
        if (record.replyTokenValue() == 0L) {
            return null;
        }
        RoutingId rid = ContractAccess.routingIdFromTrusted(
            record.routingIdBytes());
        return replySender(rid, record.replyTokenValue());
    }

    private BiConsumer<List<Message>, SendFlags> replySender(
            RoutingId nodeRid, long replyTokenValue) {
        return (replyParts, sendFlags) -> {
            if (socket instanceof NativeRouterSocket nativeSocket) {
                nativeSocket.submitReply(nodeRid, replyTokenValue, replyParts);
                return;
            }
            InternalAccess.routerReply(socket, nodeRid, replyTokenValue,
                replyParts);
        };
    }
}
