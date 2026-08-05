/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.internal.ContractAccess;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.contracts.sockets.RecvFlags;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeCallbackSupport;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeMessage;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.RuntimeResources;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.util.ArrayList;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.function.BiConsumer;

final class NativeRouterReceiveSupport implements AutoCloseable {
    private static final Linker LINKER = Linker.nativeLinker();
    // Matches zlink_socket_msg_handler_fn:
    //   (const zlink_routing_id_t* source_rid, zlink_msg_t* parts,
    //    size_t part_count, void* userdata) -> void
    private static final FunctionDescriptor FD_RECV_HANDLER =
      FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
        ValueLayout.JAVA_LONG, ValueLayout.ADDRESS);

    private final NativeRouterSocket socket;
    private final boolean closeSocketOnClose;
    private volatile SocketMessageHandler dataHandler;
    private final NativeCallbackSupport callbacks =
        new NativeCallbackSupport("zlink-router-callback");
    private Arena receiveCallbackArena;
    private boolean handlerRegistered;
    private volatile boolean closed;
    private static final ThreadLocal<RecvOutScratch> RECV_OUT_SCRATCH =
        ThreadLocal.withInitial(RecvOutScratch::new);

    static final class RecvOutScratch {
        final MemorySegment sourceNodeRidOut;
        final MemorySegment requestSeqOut;
        final MemorySegment partsOut;
        final MemorySegment partCountOut;
        final MemorySegment hasMoreOut;

        RecvOutScratch() {
            Arena auto = Arena.ofAuto();
            sourceNodeRidOut = auto.allocate(ValueLayout.ADDRESS);
            requestSeqOut = auto.allocate(ValueLayout.JAVA_LONG);
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
        if (dataHandler != null) {
            throw new IllegalStateException(
                "socket is in callback mode; direct recv is not allowed");
        }
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

    public void onReceive(SocketMessageHandler handler) {
        Objects.requireNonNull(handler, "handler");
        ensureOpen();
        callbacks.ensureNoFailure();

        NativeCallbackSupport.ExecutorLease lease = callbacks.ensureExecutor();
        ExecutorService executor = lease.executor();

        Arena arena = null;
        boolean createdHandler = false;
        if (!handlerRegistered) {
            // Router receive keeps one native callback stub installed and swaps
            // the Java handler reference. The native side can keep a stable
            // function pointer while callers replace the Java callback.
            arena = Arena.ofShared();
            MemorySegment stub = LINKER.upcallStub(callbackHandle(
                "handleReceiveCallback",
                MethodType.methodType(void.class, MemorySegment.class,
                    MemorySegment.class, long.class, MemorySegment.class)),
                FD_RECV_HANDLER, arena);
            int rc = NativeMessage.recvHandler(InternalAccess.socketHandle(socket), stub,
                MemorySegment.NULL);
            if (rc != 0) {
                callbacks.clearExecutorIfCreated(lease);
                RuntimeResources.closeArena(arena);
                throw ZlinkException.fromLastError(systems.zlink.contracts.errors.ErrorCategory.HANDLER);
            }
            receiveCallbackArena = arena;
            handlerRegistered = true;
            createdHandler = true;
        }

        try {
            dataHandler = handler;
        } catch (RuntimeException ex) {
            if (createdHandler) {
                RuntimeResources.closeArena(receiveCallbackArena);
                receiveCallbackArena = null;
                handlerRegistered = false;
            }
            callbacks.clearExecutorIfCreated(lease);
            throw ex;
        }
    }

    Received recvNoWaitOrNull() {
        if (dataHandler != null) {
            throw new IllegalStateException(
                "socket is in callback mode; direct recv is not allowed");
        }
        return recvDirectOnceOrNull(RecvFlags.DONT_WAIT);
    }

    /**
     * Receives into caller-provided {@link Received} storage.
     *
     * <p>HOT PATH: single-part routed messages without request metadata fill
     * {@code target} directly and avoid a fresh {@link Received} allocation.
     * Multipart and request/reply messages use the fallback path so observable
     * recv semantics stay identical across message shapes.
     * Returns {@code true} on data, {@code false} on EAGAIN with
     * {@link RecvFlags#DONT_WAIT}.
     */
    public boolean recvInto(Received target, RecvFlags flags) {
        Objects.requireNonNull(target, "target");
        Objects.requireNonNull(flags, "flags");
        if (dataHandler != null) {
            throw new IllegalStateException(
                "socket is in callback mode; direct recv is not allowed");
        }
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
        dataHandler = null;
        callbacks.close(1, TimeUnit.SECONDS);
    }

    public void finishClose() {
        RuntimeResources.closeArena(receiveCallbackArena);
        receiveCallbackArena = null;
    }

    // zlink_socket_msg_handler_fn upcall. The Core raw API delivers callback receive
    // only for raw STREAM subjects, which carry no request/reply metadata, so
    // the callback shape is (source_rid, parts, part_count, userdata) with no
    // request sequence. Ownership of the parts vector transfers
    // to us; snapshotReceive materialises and closes it exactly once.
    private void handleReceiveCallback(MemorySegment sourceRid,
                                       MemorySegment parts,
                                       long partCount,
                                       MemorySegment userdata) {
        SocketMessageHandler handler = dataHandler;
        ExecutorService executor = callbacks.executor();
        if (handler == null || executor == null) {
            NativeMessage.multipartClose(parts, partCount);
            return;
        }
        CallbackReceivedData snapshot;
        try {
            snapshot = snapshotReceive(sourceRid, parts, partCount);
        } catch (RuntimeException ex) {
            callbacks.recordFailure(ex);
            return;
        }
        try {
            executor.execute(() -> dispatchReceive(handler, snapshot));
        } catch (RejectedExecutionException ex) {
            callbacks.recordFailure(ex);
        }
    }

    private CallbackReceivedData snapshotReceive(MemorySegment sourceRid,
                                                 MemorySegment parts,
                                                 long partCount) {
        Message[] snapshotParts;
        try {
            snapshotParts = InternalAccess.messageFromOwnedMessageVectorShared(parts, partCount);
        } finally {
            NativeMessage.multipartClose(parts, partCount);
        }
        return new CallbackReceivedData(NativeRoutingIds.read(sourceRid),
            snapshotParts);
    }

    private void dispatchReceive(SocketMessageHandler handler,
                                 CallbackReceivedData snapshot) {
        RoutingId nodeRid = snapshot.nodeRid();
        // STREAM callback receive has no request sequence or reply
        // channel; deliver a plain routed Received keyed only by the source rid.
        try (Received received = InternalAccess.received(nodeRid,
            snapshot.parts(), true, 0L, false, null)) {
            handler.onMessage(received);
        } catch (RuntimeException ex) {
            callbacks.recordFailure(ex);
        }
    }

    /**
     * Variant of {@link #recvDirectOnceImpl} for caller-provided storage.
     *
     * <p>HOT PATH: single-part routed messages without request metadata
     * populate {@code target} via {@link Received#populateRoutedSinglePart},
     * avoiding the fresh {@link Received} allocation used by the fallback path.
     * Other paths use the existing receive-and-adopt flow.
     */
    private boolean recvDirectOnceIntoImpl(Received target, RecvFlags flags,
                                           boolean nullOnNoData) {
        RecvOutScratch scratch = RECV_OUT_SCRATCH.get();
        MemorySegment sourceNodeRidOut = scratch.sourceNodeRidOut;
        MemorySegment requestSeqOut = scratch.requestSeqOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        Message firstPart = new Message();
        boolean firstPartConsumed = false;
        try {
            int rc;
            while (true) {
                rc = routerRecvPart(sourceNodeRidOut,
                    requestSeqOut,
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
            long requestSequence = requestSeqOut.get(ValueLayout.JAVA_LONG, 0);

            if (!hasMore && requestSequence == 0L) {
                // Routed echo hot path: populate caller storage in place.
                byte[] nodeRidBytes =
                    NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                firstPartConsumed = true;
                ContractAccess.receivedPopulateRoutedSinglePart(target,
                    nodeRidBytes, firstPart, 0L, false, null,
                    null);
                if (nodeRidBytes != null) {
                    socket.attachSendSender(target);
                }
                return true;
            }

            // Cold path (multipart or request-seq): fall back to the
            // allocate-and-adopt implementation so surface semantics stay
            // identical for non-echo routed recv shapes (request-reply,
            // multipart envelopes).
            firstPartConsumed = continueFallbackAdopt(target, firstPart, hasMore,
                requestSequence, scratch, flags);
            return true;
        } finally {
            if (!firstPartConsumed) {
                try { firstPart.close(); } catch (RuntimeException ignored) {}
            }
        }
    }

    private boolean continueFallbackAdopt(Received target, Message firstPart,
                                          boolean hasMore, long requestSequence,
                                          RecvOutScratch scratch, RecvFlags flags) {
        // Reconstruct a fresh Received via the existing constructors for the
        // multipart / request-seq case, then adoptFrom into the target.
        MemorySegment sourceNodeRidOut = scratch.sourceNodeRidOut;
        MemorySegment requestSeqOut = scratch.requestSeqOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        Received fresh;
        if (!hasMore) {
            RoutingId nodeRid = NativeRoutingIds.readOut(sourceNodeRidOut);
            fresh = InternalAccess.receivedLazy(nodeRid, firstPart,
                null, requestSequence, true,
                replySender(nodeRid, requestSequence),
                null);
        } else {
            ArrayList<Message> parts = new ArrayList<>();
            parts.add(firstPart);
            Message[] partsArray = recvRemainingMultipartParts(parts, scratch,
                flags);
            if (requestSequence == 0L) {
                byte[] nodeRidBytes =
                    NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                fresh = InternalAccess.received(nodeRidBytes, partsArray,
                    true, 0L, false, null, null);
            } else {
                RoutingId nodeRid =
                    NativeRoutingIds.readOut(sourceNodeRidOut);
                fresh = InternalAccess.received(nodeRid, partsArray,
                    true, requestSequence, true,
                    replySender(nodeRid, requestSequence),
                    null);
            }
        }
        ContractAccess.receivedAdoptFrom(target, fresh);
        return true;
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
        MemorySegment requestSeqOut = scratch.requestSeqOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        Message firstPart = new Message();
        boolean firstPartConsumed = false;
        try {
            int rc;
            while (true) {
                rc = routerRecvPart(sourceNodeRidOut,
                    requestSeqOut,
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
            long requestSequence = requestSeqOut.get(ValueLayout.JAVA_LONG, 0);

            if (!hasMore) {
                firstPartConsumed = true;
                if (requestSequence == 0L) {
                    byte[] nodeRidBytes =
                        NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                    return InternalAccess.receivedLazy(nodeRidBytes, firstPart,
                        null,
                        0L, false, null, null);
                }
                RoutingId nodeRid =
                    NativeRoutingIds.readOut(sourceNodeRidOut);
                return InternalAccess.receivedLazy(nodeRid, firstPart,
                    null, requestSequence, true,
                    replySender(nodeRid, requestSequence),
                    null);
            }

            ArrayList<Message> parts = new ArrayList<>();
            parts.add(firstPart);
            Message[] partsArray = recvRemainingMultipartParts(parts, scratch,
                flags);
            firstPartConsumed = true;
            if (requestSequence == 0L) {
                byte[] nodeRidBytes =
                    NativeRoutingIds.readBytesOut(sourceNodeRidOut);
                return InternalAccess.received(nodeRidBytes, partsArray, true,
                    0L, false, null, null);
            }
            RoutingId nodeRid = NativeRoutingIds.readOut(sourceNodeRidOut);
            return InternalAccess.received(nodeRid, partsArray, true,
                requestSequence, true,
                replySender(nodeRid, requestSequence),
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
        MemorySegment requestSeqOut = scratch.requestSeqOut;
        MemorySegment hasMoreOut = scratch.hasMoreOut;
        boolean hasMore = true;
        while (hasMore) {
            Message next = new Message();
            boolean nextOk = false;
            try {
                int rc = routerRecvPart(sourceNodeRidOut,
                    requestSeqOut, InternalAccess.messageNativeHandle(next),
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
      RoutingId nodeRid, long requestSequence) {
        if (requestSequence == 0L) {
            return null;
        }
        return (replyParts, sendFlags) ->
            InternalAccess.routerReply(socket, nodeRid, requestSequence,
                replyParts);
    }

    private int routerRecvPart(MemorySegment sourceNodeRidOut,
                               MemorySegment requestSeqOut,
                               MemorySegment partOut,
                               MemorySegment hasMoreOut,
                               int flags) {
        return Native.routerRecvPart(InternalAccess.socketHandle(socket), sourceNodeRidOut,
            requestSeqOut, partOut, hasMoreOut, flags);
    }

    private MethodHandle callbackHandle(String name, MethodType type) {
        try {
            return MethodHandles.lookup().findVirtual(
                NativeRouterReceiveSupport.class, name, type).bindTo(this);
        } catch (ReflectiveOperationException ex) {
            throw new IllegalStateException("failed to bind callback " + name, ex);
        }
    }

    private void ensureOpen() {
        if (closed || InternalAccess.socketHandle(socket) == null || InternalAccess.socketHandle(socket).address() == 0) {
            throw new IllegalStateException("socket is closed");
        }
    }

    private record CallbackReceivedData(RoutingId nodeRid, Message[] parts) {}
}
