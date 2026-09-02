/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantReadWriteLock;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.CompletionDispatcher;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;

/** Socket-local owner for Core 0.16 pull completions. */
final class CompletionOwner implements AutoCloseable {
    private static final int COMPLETION_SEND = 1;
    private static final int COMPLETION_REQUEST = 2;
    private static final int SEND_ADMITTED = 0;
    private static final int RECV_DONT_WAIT = 1;
    private static final AtomicLong NEXT_CONTEXT = new AtomicLong(1L);
    private static final AtomicLong CLOSED_COMPLETIONS = new AtomicLong();

    private final NativeSocketRuntime socket;
    private final CompletionDispatcher.CompletionLane lane;
    private final ConcurrentHashMap<Long, Pending<?>> pending =
        new ConcurrentHashMap<>();
    private final Object ownerLock = new Object();
    private final Object drainLock = new Object();
    private final ReentrantReadWriteLock nativeCallGate =
        new ReentrantReadWriteLock();
    private volatile boolean closed;
    private boolean finalized;
    private boolean publicOwner;
    private Thread runtimeOwner;

    CompletionOwner(NativeSocketRuntime socket,
                    CompletionDispatcher.CompletionLane lane) {
        this.socket = Objects.requireNonNull(socket, "socket");
        this.lane = Objects.requireNonNull(lane, "lane");
    }

    CompletionStage<Void> submitSend(RoutingId target, List<Message> parts) {
        Pending<Void> state = register(false);
        try {
            long id = submitParts(target, parts, SendFlags.DONT_WAIT.value(),
                0, state.context(), true, false, 0L);
            state.publish(id);
            return state.future;
        } catch (RuntimeException | Error failure) {
            state.reject(failure);
            throw failure;
        }
    }

    void submitSendBlocking(RoutingId target, List<Message> parts) {
        submitParts(target, parts, SendFlags.NONE.value(), 0,
            MemorySegment.NULL, false, false, 0L);
    }

    CompletionStage<List<Message>> submitRequest(RoutingId target,
                                                  List<Message> parts,
                                                  Duration timeout) {
        Pending<List<Message>> state = register(true);
        try {
            long id = submitParts(target, parts, SendFlags.DONT_WAIT.value(),
                timeoutMillis(timeout), state.context(), true, true, 0L);
            if (id == 0L) {
                throw new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
            }
            state.publish(id);
            return state.future;
        } catch (RuntimeException | Error failure) {
            state.reject(failure);
            throw failure;
        }
    }

    List<Message> submitRequestBlocking(RoutingId target,
                                        List<Message> parts,
                                        Duration timeout) {
        try {
            return submitRequestWithFlags(target, parts, timeout,
                SendFlags.NONE.value()).toCompletableFuture().join();
        } catch (java.util.concurrent.CompletionException failure) {
            if (failure.getCause() instanceof RuntimeException runtime) {
                throw runtime;
            }
            throw failure;
        }
    }

    private CompletionStage<List<Message>> submitRequestWithFlags(
            RoutingId target, List<Message> parts, Duration timeout, int flags) {
        Pending<List<Message>> state = register(true);
        try {
            long id = submitParts(target, parts, flags, timeoutMillis(timeout),
                state.context(), true, true, 0L);
            if (id == 0L) {
                throw new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
            }
            state.publish(id);
            return state.future;
        } catch (RuntimeException | Error failure) {
            state.reject(failure);
            throw failure;
        }
    }

    void submitReply(RoutingId target, long token, List<Message> parts) {
        submitParts(target, parts, SendFlags.NONE.value(), 0,
            MemorySegment.NULL, false, false, token);
    }

    private long submitParts(RoutingId target, List<Message> originals,
                             int flags, int timeoutMs,
                             MemorySegment userContext, boolean completion,
                             boolean request, long replyToken) {
        ReentrantReadWriteLock.ReadLock read = nativeCallGate.readLock();
        read.lock();
        try {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            return submitPartsLocked(target, originals, flags, timeoutMs,
                userContext, completion, request, replyToken);
        } finally {
            read.unlock();
        }
    }

    private long submitPartsLocked(RoutingId target, List<Message> originals,
                                   int flags, int timeoutMs,
                                   MemorySegment userContext,
                                   boolean completion, boolean request,
                                   long replyToken) {
        Objects.requireNonNull(originals, "parts");
        if (originals.isEmpty()) {
            throw new IllegalArgumentException("at least one message required");
        }
        for (int i = 0; i < originals.size(); i++) {
            Objects.requireNonNull(originals.get(i), "parts[" + i + "]");
        }
        List<Message> staged = new ArrayList<>(originals.size());
        for (Message original : originals) {
            staged.add(InternalAccess.messageSharedCopyOf(
                original));
        }
        try (Arena arena = Arena.ofConfined()) {
            MemorySegment nativeTarget = target == null ? MemorySegment.NULL
                : NativeRoutingIds.allocate(arena, target);
            MemorySegment idOut = completion
                ? arena.allocate(ValueLayout.JAVA_LONG) : MemorySegment.NULL;
            for (int i = 0; i < staged.size(); i++) {
                Message copy = staged.get(i);
                MemorySegment nativePart = arena.allocate(
                    NativeLayouts.MESSAGE_LAYOUT);
                InternalAccess.messageTransferTo(copy, nativePart);
                int partFlag = i + 1 < staged.size()
                    ? Native.PART_MORE : Native.PART_FINAL;
                boolean last = partFlag == Native.PART_FINAL;
                MemorySegment context = last ? userContext : MemorySegment.NULL;
                MemorySegment output = last ? idOut : MemorySegment.NULL;
                int rc;
                if (!request && replyToken == 0L) {
                    rc = target == null
                        ? Native.sendPart(socket.handle(), nativePart, flags,
                            partFlag, context, output)
                        : Native.sendPartRid(socket.handle(), nativeTarget,
                            nativePart, flags, partFlag, context, output);
                } else if (request) {
                    rc = Native.requestPart(socket.handle(), nativeTarget,
                        nativePart, flags, partFlag, last ? timeoutMs : 0,
                        context, output);
                } else {
                    rc = Native.replyPart(socket.handle(), nativeTarget,
                        replyToken, nativePart, partFlag);
                }
                if (rc != SubmitResult.OK.value()) {
                    closeRemaining(staged, i + 1);
                    throw new ZlinkSubmitException(SubmitResult.fromValue(rc),
                        Native.errno());
                }
            }
            for (Message original : originals) {
                InternalAccess.messageMarkTransferred(original);
            }
            return completion ? idOut.get(ValueLayout.JAVA_LONG, 0) : 0L;
        } catch (RuntimeException | Error failure) {
            closeRemaining(staged, 0);
            throw failure;
        }
    }

    int closeNativeSocket() {
        ReentrantReadWriteLock.WriteLock write = nativeCallGate.writeLock();
        write.lock();
        try {
            synchronized (ownerLock) {
                closed = true;
            }
            int rc = Native.close(socket.handle());
            if (rc != systems.zlink.contracts.errors.CloseResult.OK.value()) {
                synchronized (ownerLock) {
                    closed = false;
                }
                startRuntimeOwner();
            }
            return rc;
        } finally {
            write.unlock();
        }
    }

    private static void closeRemaining(List<Message> staged, int from) {
        for (int i = from; i < staged.size(); i++) {
            try {
                staged.get(i).close();
            } catch (RuntimeException ignored) {
            }
        }
    }

    private <T> Pending<T> register(boolean request) {
        Pending<T> state;
        synchronized (ownerLock) {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            long token;
            do {
                token = NEXT_CONTEXT.getAndIncrement();
            } while (token == 0L || pending.containsKey(token));
            state = new Pending<>(token, request);
            pending.put(token, state);
        }
        startRuntimeOwner();
        return state;
    }

    int drain(boolean waitForSettlement) {
        int progress = 0;
        synchronized (drainLock) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment completion = arena.allocate(
                    NativeLayouts.COMPLETION_LAYOUT);
                while (!closed) {
                    completion.fill((byte) 0);
                    completion.set(ValueLayout.JAVA_INT,
                        NativeLayouts.COMPLETION_STRUCT_SIZE_OFFSET,
                        (int) NativeLayouts.COMPLETION_LAYOUT.byteSize());
                    int rc = Native.completionRecv(socket.handle(), completion,
                        RECV_DONT_WAIT);
                    if (rc == RecvResult.NO_DATA.value()
                            || rc == RecvResult.BUSY.value()) {
                        break;
                    }
                    if (rc != RecvResult.OK.value()) {
                        throw new systems.zlink.contracts.errors.ZlinkRecvException(
                            RecvResult.fromValue(rc), Native.errno());
                    }
                    progress++;
                    Pending<?> state = null;
                    try {
                        MemorySegment context = completion.get(
                            ValueLayout.ADDRESS,
                            NativeLayouts.COMPLETION_CONTEXT_OFFSET);
                        state = pending.get(context.address());
                        if (state != null) {
                            state.capture(readResult(completion, state.request));
                        }
                    } finally {
                        Native.completionClose(completion);
                        CLOSED_COMPLETIONS.incrementAndGet();
                    }
                    if (waitForSettlement && state != null) {
                        state.awaitSettlement();
                    }
                }
            }
        }
        return progress;
    }

    private Object readResult(MemorySegment completion, boolean request) {
        int kind = completion.get(ValueLayout.JAVA_INT,
            NativeLayouts.COMPLETION_KIND_OFFSET);
        if (request) {
            if (kind != COMPLETION_REQUEST) {
                return new ZlinkRequestException(RequestResult.PROTOCOL_ERROR);
            }
            RequestResult result = RequestResult.fromValue(completion.get(
                ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_REQUEST_RESULT_OFFSET));
            if (result != RequestResult.OK) {
                return new ZlinkRequestException(result);
            }
            MemorySegment parts = completion.get(ValueLayout.ADDRESS,
                NativeLayouts.COMPLETION_REPLY_PARTS_OFFSET);
            long count = completion.get(ValueLayout.JAVA_LONG,
                NativeLayouts.COMPLETION_REPLY_COUNT_OFFSET);
            Message[] replies = InternalAccess.messageFromOwnedMessageVectorShared(
                parts, count);
            return List.copyOf(Arrays.asList(replies));
        }
        if (kind != COMPLETION_SEND) {
            return new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
        }
        int result = completion.get(ValueLayout.JAVA_INT,
            NativeLayouts.COMPLETION_SEND_RESULT_OFFSET);
        if (result != SEND_ADMITTED) {
            int errno = completion.get(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_SEND_ERRNO_OFFSET);
            return new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR, errno);
        }
        return null;
    }

    void transferToPublic() {
        Thread join;
        synchronized (ownerLock) {
            publicOwner = true;
            join = runtimeOwner;
            runtimeOwner = null;
        }
        if (join != null) {
            join.interrupt();
            try {
                join.join();
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new IllegalStateException(interrupted);
            }
        }
    }

    void releasePublic() {
        synchronized (ownerLock) {
            publicOwner = false;
        }
        startRuntimeOwner();
    }

    private void startRuntimeOwner() {
        synchronized (ownerLock) {
            if (closed || publicOwner || pending.isEmpty()
                    || runtimeOwner != null) {
                return;
            }
            runtimeOwner = Thread.ofVirtual()
                .name("zlink-completion-owner")
                .start(this::runtimeDrainLoop);
        }
    }

    private void runtimeDrainLoop() {
        try {
            while (!closed && !Thread.currentThread().isInterrupted()) {
                synchronized (ownerLock) {
                    if (publicOwner || pending.isEmpty()) {
                        return;
                    }
                }
                if (drain(false) == 0) {
                    Thread.sleep(1L);
                }
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
        } finally {
            boolean restart;
            synchronized (ownerLock) {
                if (runtimeOwner == Thread.currentThread()) {
                    runtimeOwner = null;
                }
                restart = !closed && !publicOwner && !pending.isEmpty();
            }
            if (restart) {
                startRuntimeOwner();
            }
        }
    }

    @Override
    public void close() {
        synchronized (ownerLock) {
            if (finalized) {
                return;
            }
            finalized = true;
            closed = true;
        }
        transferToPublic();
        IllegalStateException failure = new IllegalStateException(
            "socket is closed");
        for (Pending<?> state : pending.values()) {
            state.reject(failure);
        }
        pending.clear();
    }

    private static int timeoutMillis(Duration timeout) {
        Objects.requireNonNull(timeout, "timeout");
        if (timeout.isNegative() || timeout.toMillis() > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("timeout is out of range");
        }
        return (int) timeout.toMillis();
    }

    static long closedCompletionCount() {
        return CLOSED_COMPLETIONS.get();
    }

    private final class Pending<T> {
        private final long token;
        private final boolean request;
        private final CompletableFuture<T> future = new CompletableFuture<>();
        private boolean published;
        private boolean captured;
        private boolean dispatched;
        private Object result;

        Pending(long token, boolean request) {
            this.token = token;
            this.request = request;
        }

        MemorySegment context() {
            return MemorySegment.ofAddress(token);
        }

        synchronized void publish(long id) {
            published = true;
            if (!request && id == 0L && !captured) {
                captured = true;
                result = null;
                dispatched = true;
                pending.remove(token, this);
                future.complete(null);
                notifyAll();
                return;
            }
            dispatchIfReady();
        }

        synchronized void capture(Object value) {
            captured = true;
            result = value;
            dispatchIfReady();
        }

        void reject(Throwable failure) {
            pending.remove(token, this);
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                dispatched = true;
            }
            lane.dispatch(() -> future.completeExceptionally(failure));
        }

        private void dispatchIfReady() {
            if (!published || !captured || dispatched) {
                return;
            }
            dispatched = true;
            pending.remove(token, this);
            Object value = result;
            lane.dispatch(() -> {
                try {
                    if (value instanceof Throwable failure) {
                        future.completeExceptionally(failure);
                    } else {
                        @SuppressWarnings("unchecked")
                        T typed = (T) value;
                        future.complete(typed);
                    }
                } finally {
                    synchronized (Pending.this) {
                        Pending.this.notifyAll();
                    }
                }
            });
        }

        synchronized void awaitSettlement() {
            while (!future.isDone()) {
                try {
                    wait();
                } catch (InterruptedException interrupted) {
                    Thread.currentThread().interrupt();
                    return;
                }
            }
        }
    }
}
