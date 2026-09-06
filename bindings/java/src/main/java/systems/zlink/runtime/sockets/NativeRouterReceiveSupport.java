/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.util.ArrayList;
import java.util.Objects;
import java.util.Optional;
import java.util.function.BiConsumer;

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
        final MemorySegment hasMoreOut;

        RecvOutScratch() {
            Arena auto = Arena.ofAuto();
            sourceNodeRidOut = auto.allocate(ValueLayout.ADDRESS);
            replyTokenValueOut = auto.allocate(ValueLayout.JAVA_LONG);
            partsOut = auto.allocate(ValueLayout.ADDRESS);
            partCountOut = auto.allocate(ValueLayout.JAVA_LONG);
            hasMoreOut = auto.allocate(ValueLayout.JAVA_INT);
        }
    }

    NativeRouterReceiveSupport(RouterSocket socket) {
        this(socket, true);
    }

    NativeRouterReceiveSupport(RouterSocket socket, boolean closeSocketOnClose) {
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
        if (flags == RecvFlags.DONT_WAIT) {
            Received received = recvNoWaitOrNull();
            if (received == null) {
                throw new ZlinkRecvException(RecvResult.NO_DATA,
                    NativeErrno.EAGAIN);
            }
            return received;
        }
        return recvDirect(flags);
    }

    Received recvNoWaitOrNull() {
        return recvDirectOnceOrNull(RecvFlags.DONT_WAIT);
    }

    /**
     * Receives into caller-provided {@link Received} storage.
     *
     * Completed parts and routing metadata populate the target directly.
     * Returns {@code true} on data, {@code false} on EAGAIN with
     * {@link RecvFlags#DONT_WAIT}.
     */
    public boolean recvInto(Received target, RecvFlags flags) {
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(flags, "flags");
        boolean dontWait = (flags == RecvFlags.DONT_WAIT);
        return recvDirectOnceIntoImpl(target, flags, dontWait);
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
        if (closed) {
            return;
        }
        closed = true;
    }

    public void finishClose() {
    }

    private boolean recvDirectOnceIntoImpl(Received target, RecvFlags flags,
                                           boolean nullOnNoData) {
        RecvOutScratch scratch = RECV_OUT_SCRATCH.get();
        MemorySegment sourceNodeRidOut = scratch.sourceNodeRidOut;
        MemorySegment replyTokenValueOut = scratch.replyTokenValueOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        Message firstPart = InternalAccess.messageAcquireReceive();
        boolean firstPartConsumed = false;
        try {
            int rc;
            while (true) {
                rc = routerRecvPart(sourceNodeRidOut,
                    replyTokenValueOut,
                    InternalAccess.messageNativeHandle(firstPart), hasMoreOut,
                    flags.value());
                if (rc == 0) break;
                int errno = Native.errno();
                if (errno == NativeErrno.EINTR) continue;
                RecvResult result = RecvResult.fromValue(rc);
                if (nullOnNoData && (result == RecvResult.NO_DATA
                    || result == RecvResult.BUSY)) {
                    return false;
                }
                throw new ZlinkRecvException(result, errno);
            }
            boolean hasMore = hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
            InternalAccess.messageFinishReceive(firstPart, hasMore);
            long replyTokenValue = replyTokenValueOut.get(ValueLayout.JAVA_LONG, 0);

            if (!hasMore) {
                // A caller-provided Received can retain both plain routed
                // messages and request/reply metadata without allocating an
                // intermediate Received object.
                byte[] nodeRidBytes =
                    NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                firstPartConsumed = true;
                ContractAccess.receivedPopulateRoutedSinglePart(target,
                    nodeRidBytes, firstPart, replyTokenValue,
                    replyTokenValue != 0L, null, null);
                return true;
            }

            Message secondPart = InternalAccess.messageAcquireReceive();
            boolean secondPartConsumed = false;
            try {
                while (true) {
                    int nextRc = routerRecvPart(sourceNodeRidOut, replyTokenValueOut,
                        InternalAccess.messageNativeHandle(secondPart),
                        hasMoreOut, flags.value());
                    if (nextRc == 0) {
                        break;
                    }
                    int errno = Native.errno();
                    if (errno == NativeErrno.EINTR) {
                        continue;
                    }
                    throw new ZlinkRecvException(RecvResult.fromValue(nextRc),
                        errno);
                }
                boolean secondHasMore = hasMoreOut.get(
                    ValueLayout.JAVA_INT, 0) != 0;
                InternalAccess.messageFinishReceive(secondPart, secondHasMore);
                if (!secondHasMore) {
                    byte[] nodeRidBytes =
                        NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                    firstPartConsumed = true;
                    secondPartConsumed = true;
                    ContractAccess.receivedPopulateRoutedTwoParts(target,
                        nodeRidBytes, firstPart, secondPart, replyTokenValue,
                        replyTokenValue != 0L, null, null);
                    return true;
                }

                ArrayList<Message> parts = new ArrayList<>();
                parts.add(firstPart);
                parts.add(secondPart);
                firstPartConsumed = true;
                secondPartConsumed = true;
                boolean adopted = false;
                try {
                    Message[] partsArray = recvRemainingMultipartParts(parts,
                        scratch, flags);
                    byte[] nodeRidBytes =
                        NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                    ContractAccess.receivedPopulateRoutedParts(target,
                        nodeRidBytes, partsArray, replyTokenValue,
                        replyTokenValue != 0L, null, null);
                    adopted = true;
                    return true;
                } finally {
                    if (!adopted)
                        Message.closeAll(parts);
                }
            } finally {
                if (!secondPartConsumed) {
                    try {
                        secondPart.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }
        } finally {
            if (!firstPartConsumed) {
                try { firstPart.close(); } catch (RuntimeException ignored) {}
            }
        }
    }

    private Received recvDirect(RecvFlags flags) {
        return recvDirectOnce(flags);
    }

    Received recvDirectOnceOrNull(RecvFlags flags) {
        return recvDirectOnceImpl(flags, true);
    }

    private Received recvDirectOnce(RecvFlags flags) {
        return recvDirectOnceImpl(flags, false);
    }

    private Received recvDirectOnceImpl(RecvFlags flags, boolean nullOnNoData) {
        RecvOutScratch scratch = RECV_OUT_SCRATCH.get();
        MemorySegment sourceNodeRidOut = scratch.sourceNodeRidOut;
        MemorySegment replyTokenValueOut = scratch.replyTokenValueOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        Message firstPart = InternalAccess.messageAcquireReceive();
        boolean firstPartConsumed = false;
        try {
            int rc;
            while (true) {
                rc = routerRecvPart(sourceNodeRidOut,
                    replyTokenValueOut,
                    InternalAccess.messageNativeHandle(firstPart), hasMoreOut,
                    flags.value());
                if (rc == 0) break;
                int errno = Native.errno();
                if (errno == NativeErrno.EINTR) continue;
                RecvResult result = RecvResult.fromValue(rc);
                if (nullOnNoData && (result == RecvResult.NO_DATA
                    || result == RecvResult.BUSY)) {
                    return null;
                }
                throw new ZlinkRecvException(result, errno);
            }
            boolean hasMore = hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
            InternalAccess.messageFinishReceive(firstPart, hasMore);
            long replyTokenValue = replyTokenValueOut.get(ValueLayout.JAVA_LONG, 0);

            if (!hasMore) {
                firstPartConsumed = true;
                if (replyTokenValue == 0L) {
                    byte[] nodeRidBytes =
                        NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                    return InternalAccess.receivedLazy(nodeRidBytes, firstPart,
                        null,
                        0L, false, null, null);
                }
                RoutingId nodeRid =
                    NativeRoutingIds.readOut(sourceNodeRidOut);
                return InternalAccess.receivedLazy(nodeRid, firstPart,
                    null, replyTokenValue, true,
                    replySender(nodeRid, replyTokenValue),
                    null);
            }

            ArrayList<Message> parts = new ArrayList<>();
            parts.add(firstPart);
            Message[] partsArray = recvRemainingMultipartParts(parts, scratch,
                flags);
            firstPartConsumed = true;
            if (replyTokenValue == 0L) {
                byte[] nodeRidBytes =
                    NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                return InternalAccess.received(nodeRidBytes, partsArray, true,
                    0L, false, null, null);
            }
            RoutingId nodeRid = NativeRoutingIds.readOut(sourceNodeRidOut);
            return InternalAccess.received(nodeRid, partsArray, true,
                replyTokenValue, true,
                replySender(nodeRid, replyTokenValue),
                null);
        } finally {
            if (!firstPartConsumed) {
                try {
                    firstPart.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    private Message[] recvRemainingMultipartParts(ArrayList<Message> parts,
                                                  RecvOutScratch scratch,
                                                  RecvFlags flags) {
        MemorySegment sourceNodeRidOut = scratch.sourceNodeRidOut;
        MemorySegment replyTokenValueOut = scratch.replyTokenValueOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        boolean hasMore = true;
        while (hasMore) {
            Message next = InternalAccess.messageAcquireReceive();
            boolean nextOk = false;
            try {
                int rc = routerRecvPart(sourceNodeRidOut,
                    replyTokenValueOut, InternalAccess.messageNativeHandle(next),
                    hasMoreOut, flags.value());
                if (rc != 0) {
                    if (Native.errno() == NativeErrno.EINTR) {
                        continue;
                    }
                    throw new ZlinkRecvException(RecvResult.fromValue(rc),
                        Native.errno());
                }
                hasMore = hasMoreOut.get(ValueLayout.JAVA_INT, 0) != 0;
                InternalAccess.messageFinishReceive(next, hasMore);
                parts.add(next);
                nextOk = true;
            } finally {
                if (!nextOk) {
                    try {
                        next.close();
                    } catch (RuntimeException ignored) {
                    }
                }
            }
        }
        return parts.toArray(new Message[0]);
    }

    private BiConsumer<java.util.List<Message>, SendFlags> replySender(
      RoutingId nodeRid, long replyTokenValue) {
        if (replyTokenValue == 0L) {
            return null;
        }
        return (replyParts, sendFlags) -> {
            if (socket instanceof NativeRouterSocket nativeSocket) {
                nativeSocket.submitReply(nodeRid, replyTokenValue, replyParts);
                return;
            }
            InternalAccess.routerReply(socket, nodeRid, replyTokenValue,
                replyParts);
        };
    }

    private int routerRecvPart(MemorySegment sourceNodeRidOut,
                               MemorySegment replyTokenValueOut,
                               MemorySegment partOut,
                               MemorySegment hasMoreOut,
                               int flags) {
        MemorySegment handle = InternalAccess.socketHandle(socket);
        if (flags == RecvFlags.DONT_WAIT.value()) {
            return Native.routerRecvPartNoWaitCritical(handle, sourceNodeRidOut,
                replyTokenValueOut, partOut, hasMoreOut, flags);
        }
        return Native.routerRecvPart(handle, sourceNodeRidOut, replyTokenValueOut,
            partOut, hasMoreOut, flags);
    }

}
