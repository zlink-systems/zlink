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
import java.util.concurrent.locks.ReentrantLock;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.CompletionKind;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.sockets.SocketOption;
import systems.zlink.runtime.nativeapi.CompletionDispatcher;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeErrno;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeMessage;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.NativeSubmitErrors;

/** Socket-local owner for Core pull completions and writable send retries. */
final class CompletionOwner implements AutoCloseable {
    private static final int SEND_ADMITTED = 0;
    private static final int SEND_TERMINAL = 202;
    private static final int RECV_DONT_WAIT = 1;
    private static final CompletionStage<Void> COMPLETED_SEND =
        CompletableFuture.completedStage(null);
    private static final AtomicLong NEXT_CONTEXT = new AtomicLong(1L);
    private static final AtomicLong CLOSED_COMPLETIONS = new AtomicLong();

    // Thread-local native headers never escape a submit/drain. Public messages
    // and stages retain their independent ownership and are not pooled here.
    private static final ThreadLocal<NativeScratch> NATIVE_SCRATCH =
        ThreadLocal.withInitial(NativeScratch::new);

    private static final class NativeScratch {
        private final Arena arena = Arena.ofAuto();
        final MemorySegment target = arena.allocate(
            NativeLayouts.ROUTING_ID_LAYOUT);
        final MemorySegment id = arena.allocate(ValueLayout.JAVA_LONG);
        final MemorySegment completion = arena.allocate(
            NativeLayouts.COMPLETION_LAYOUT);
        private MemorySegment parts = arena.allocate(
            2 * NativeLayouts.MESSAGE_LAYOUT.byteSize(),
            NativeLayouts.MESSAGE_LAYOUT.byteAlignment());

        MemorySegment parts(int count) {
            long bytes = Math.multiplyExact((long) count,
                NativeLayouts.MESSAGE_LAYOUT.byteSize());
            if (parts.byteSize() < bytes) {
                // A separate automatic arena lets an outgrown buffer be reclaimed.
                parts = Arena.ofAuto().allocate(bytes,
                    NativeLayouts.MESSAGE_LAYOUT.byteAlignment());
            }
            return parts;
        }
    }

    private final NativeSocketRuntime socket;
    private final CompletionDispatcher.CompletionLane lane;
    private final CompletionPump runtime;
    private final ConcurrentHashMap<Long, Pending<?>> pending =
        new ConcurrentHashMap<>();
    private final Object ownerLock = new Object();
    private final ReentrantLock drainLock = new ReentrantLock();
    private final ReentrantReadWriteLock nativeCallGate =
        new ReentrantReadWriteLock();
    private volatile boolean closed;
    private boolean finalized;
    private Object publicOwner;

    CompletionOwner(NativeSocketRuntime socket,
                    CompletionDispatcher.CompletionLane lane,
                    MemorySegment contextHandle) {
        this.socket = Objects.requireNonNull(socket, "socket");
        this.lane = Objects.requireNonNull(lane, "lane");
        this.runtime = lane.completionPump(contextHandle);
    }

    CompletionStage<Void> submitSend(RoutingId target, List<Message> parts) {
        drainLock.lock();
        try {
            long token = nextContextToken();
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                SendFlags.DONT_WAIT.value(), 0,
                MemorySegment.ofAddress(token), true,
                false, 0L);
            if (attempt.result() == SubmitResult.OK) {
                if (attempt.completionId() != 0L) {
                    throw new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                }
                closeParts(parts);
                return COMPLETED_SEND;
            }
            if (!isWritableWait(attempt)) {
                throw submitFailure(attempt);
            }

            List<Message> retained;
            try {
                retained = retainParts(parts);
            } catch (RuntimeException | Error failure) {
                Pending<Void> discard = registerAfterAttempt(token, target,
                    List.of(), PendingKind.DISCARD_WRITABLE);
                discard.armWritable(attempt.completionId());
                startRuntimeOwner();
                throw failure;
            }
            Pending<Void> state;
            try {
                state = registerAfterAttempt(token, target, retained,
                    PendingKind.RETRY_SEND);
            } catch (RuntimeException | Error failure) {
                closeParts(retained);
                throw failure;
            }
            state.armWritable(attempt.completionId());
            closeParts(parts);
            startRuntimeOwner();
            return state.future;
        } finally {
            drainLock.unlock();
        }
    }

    void submitSendBlocking(RoutingId target, List<Message> parts) {
        withSendSequenceLock(() -> {
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                SendFlags.NONE.value(), 0, MemorySegment.NULL, false, false,
                0L);
            requireSuccess(attempt);
            closeParts(parts);
            return null;
        });
    }

    CompletionStage<List<Message>> submitRequest(RoutingId target,
                                                  List<Message> parts,
                                                  Duration timeout) {
        drainLock.lock();
        try {
            long token = nextContextToken();
            int timeoutMs = timeoutMillis(timeout);
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                SendFlags.DONT_WAIT.value(), timeoutMs,
                MemorySegment.ofAddress(token), true, true, 0L);
            if (attempt.result() == SubmitResult.OK) {
                if (attempt.completionId() == 0L) {
                    throw new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                }
                Pending<List<Message>> state = registerRequestAfterAttempt(
                    token, target, List.of(), timeoutMs);
                state.publishRequest(attempt.completionId());
                closeParts(parts);
                startRuntimeOwner();
                return state.future;
            }
            if (!isWritableWait(attempt)) {
                throw submitFailure(attempt);
            }

            List<Message> retained;
            try {
                retained = retainParts(parts);
            } catch (RuntimeException | Error failure) {
                Pending<Void> discard = registerAfterAttempt(token, target,
                    List.of(), PendingKind.DISCARD_WRITABLE);
                discard.armWritable(attempt.completionId());
                startRuntimeOwner();
                throw failure;
            }
            Pending<List<Message>> state;
            try {
                state = registerRequestAfterAttempt(token, target, retained,
                    timeoutMs);
            } catch (RuntimeException | Error failure) {
                closeParts(retained);
                throw failure;
            }
            state.armWritable(attempt.completionId());
            closeParts(parts);
            startRuntimeOwner();
            return state.future;
        } finally {
            drainLock.unlock();
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
        Pending<List<Message>> state = null;
        try {
            state = registerRequest();
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                flags, timeoutMillis(timeout), state.context(), true,
                true, 0L);
            requireRequestSuccess(attempt);
            closeParts(parts);
            if (attempt.completionId() == 0L) {
                throw new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR);
            }
            state.publishRequest(attempt.completionId());
        } catch (RuntimeException | Error failure) {
            if (state != null)
                state.reject(failure);
            throw failure;
        }
        return state.future;
    }

    void submitReply(RoutingId target, long token, List<Message> parts) {
        withSendSequenceLock(() -> {
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                SendFlags.NONE.value(), 0, MemorySegment.NULL, false, false,
                token);
            requireSuccess(attempt);
            closeParts(parts);
            return null;
        });
    }

    <T> T withSendSequenceLock(NativeSendAction<T> action) {
        Objects.requireNonNull(action, "action");
        drainLock.lock();
        try {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            return action.run();
        } finally {
            drainLock.unlock();
        }
    }

    void markTargetRemoved(RoutingId target) {
        // A WRITABLE record may already be queued when disconnectRid wins.
        // Mark the Java-owned retry state before it can consume that stale
        // wake and reclassify its retry as a fresh missing-route failure.
        for (Pending<?> state : new ArrayList<>(pending.values())) {
            if (Objects.equals(state.target, target))
                state.markTargetRemoved();
        }
    }

    NoWaitAttempt trackNoWaitSend(RoutingId target,
                                   NoWaitSubmitter submitter) {
        Objects.requireNonNull(submitter, "submitter");
        SubmitAttempt attempt;
        drainLock.lock();
        try {
            long token = nextContextToken();
            ReentrantReadWriteLock.ReadLock read =
                nativeCallGate.readLock();
            read.lock();
            try {
                if (closed) {
                    throw new IllegalStateException("socket is closed");
                }
                try (Arena arena = Arena.ofConfined()) {
                    MemorySegment idOut = arena.allocate(
                        ValueLayout.JAVA_LONG);
                    int result = submitter.submit(
                        MemorySegment.ofAddress(token), idOut);
                    int errno = result == SubmitResult.OK.value()
                        ? 0 : Native.errno();
                    attempt = new SubmitAttempt(
                        SubmitResult.fromValue(result), errno,
                        idOut.get(ValueLayout.JAVA_LONG, 0));
                }
                if (attempt.result() == SubmitResult.OK) {
                    if (attempt.completionId() != 0L) {
                        throw new ZlinkSubmitException(
                            SubmitResult.INTERNAL_ERROR);
                    }
                } else if (isWritableWait(attempt)) {
                    Pending<Void> state = registerAfterAttempt(token, target,
                        List.of(), PendingKind.DISCARD_WRITABLE);
                    state.armWritable(attempt.completionId());
                }
            } finally {
                read.unlock();
            }
        } catch (RuntimeException | Error failure) {
            throw failure;
        } finally {
            drainLock.unlock();
        }
        if (isWritableWait(attempt))
            startRuntimeOwner();
        return new NoWaitAttempt(attempt.result().value(), attempt.errno());
    }

    private SubmitAttempt submitPartsAttempt(
            RoutingId target, List<Message> originals, int flags,
            int timeoutMs, MemorySegment userContext, boolean completion,
            boolean request, long replyToken) {
        ReentrantReadWriteLock.ReadLock read = nativeCallGate.readLock();
        read.lock();
        try {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            return submitPartsAttemptLocked(target, originals, flags,
                timeoutMs, userContext, completion, request, replyToken);
        } finally {
            read.unlock();
        }
    }

    private SubmitAttempt submitPartsAttemptLocked(
            RoutingId target, List<Message> originals, int flags,
            int timeoutMs, MemorySegment userContext, boolean completion,
            boolean request, long replyToken) {
        validateParts(originals);
        NativeScratch scratch = NATIVE_SCRATCH.get();
        MemorySegment nativeTarget = target == null ? MemorySegment.NULL
            : scratch.target;
        if (target != null) {
            NativeRoutingIds.write(nativeTarget, target);
        }
        MemorySegment idOut = completion ? scratch.id : MemorySegment.NULL;
        if (completion) {
            idOut.set(ValueLayout.JAVA_LONG, 0, 0L);
        }
        long partSize = NativeLayouts.MESSAGE_LAYOUT.byteSize();
        // Stage native headers together. zlink_msg_copy shares large
        // payload storage by refcount and avoids a Message/Arena pair per
        // part; Core consumes each staged header when it is submitted.
        MemorySegment nativeParts = scratch.parts(originals.size());
        int initialized = 0;
        int consumed = 0;
        try {
            for (; initialized < originals.size(); initialized++) {
                InternalAccess.messageCopyTo(originals.get(initialized),
                    nativeParts.asSlice(partSize * initialized, partSize));
            }
            for (; consumed < originals.size(); consumed++) {
                MemorySegment nativePart = nativeParts.asSlice(
                    partSize * consumed, partSize);
                int partFlag = consumed + 1 < originals.size()
                    ? Native.PART_MORE : Native.PART_FINAL;
                boolean last = partFlag == Native.PART_FINAL;
                MemorySegment context = last
                    ? userContext : MemorySegment.NULL;
                MemorySegment output = last
                    ? idOut : MemorySegment.NULL;
                int rc;
                if (!request && replyToken == 0L) {
                    rc = target == null
                        ? Native.sendPart(socket.handle(), nativePart,
                            flags, partFlag, context, output)
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
                    int errno = Native.errno();
                    long id = completion
                        ? idOut.get(ValueLayout.JAVA_LONG, 0) : 0L;
                    consumed++;
                    return new SubmitAttempt(SubmitResult.fromValue(rc),
                        errno, id);
                }
            }
            long id = completion
                ? idOut.get(ValueLayout.JAVA_LONG, 0) : 0L;
            return new SubmitAttempt(SubmitResult.OK, 0, id);
        } finally {
            for (int index = consumed; index < initialized; index++) {
                NativeMessage.messageClose(nativeParts.asSlice(
                    partSize * index, partSize));
            }
        }
    }

    int closeNativeSocket() {
        drainLock.lock();
        try {
            synchronized (ownerLock) {
                closed = true;
                ownerLock.notifyAll();
            }
        } finally {
            drainLock.unlock();
        }
        // Native close rejects an admitted poller wait with EBUSY. Quiesce
        // the binding-owned wait before closing its socket, as on handover.
        try {
            runtime.unregister(this);
        } catch (RuntimeException | Error failure) {
            reopenAfterCloseFailure(failure);
            throw failure;
        }
        drainLock.lock();
        try {
            ReentrantReadWriteLock.WriteLock write = nativeCallGate.writeLock();
            write.lock();
            try {
                int rc;
                try {
                    rc = Native.close(socket.handle());
                } catch (RuntimeException | Error failure) {
                    reopenAfterCloseFailure(failure);
                    throw failure;
                }
                if (rc != systems.zlink.contracts.errors.CloseResult.OK.value()) {
                    reopenAfterCloseFailure(null);
                    return rc;
                }
            } finally {
                write.unlock();
            }
        } finally {
            drainLock.unlock();
        }
        return systems.zlink.contracts.errors.CloseResult.OK.value();
    }

    private void reopenAfterCloseFailure(Throwable originalFailure) {
        synchronized (ownerLock) {
            closed = false;
            ownerLock.notifyAll();
        }
        try {
            startRuntimeOwner();
        } catch (RuntimeException | Error restartFailure) {
            if (originalFailure != null) {
                originalFailure.addSuppressed(restartFailure);
            }
        }
    }

    private static void validateParts(List<Message> parts) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty()) {
            throw new IllegalArgumentException("at least one message required");
        }
        for (int i = 0; i < parts.size(); i++) {
            if (parts.get(i) == null) {
                throw new NullPointerException("parts[" + i + "]");
            }
        }
    }

    private static List<Message> retainParts(List<Message> originals) {
        validateParts(originals);
        List<Message> retained = new ArrayList<>(originals.size());
        try {
            for (Message original : originals) {
                retained.add(InternalAccess.messageSharedCopyOf(original));
            }
            return retained;
        } catch (RuntimeException | Error failure) {
            closeParts(retained);
            throw failure;
        }
    }

    private static void closeParts(List<Message> parts) {
        closeRemaining(parts, 0);
    }

    private static void closeRemaining(List<Message> parts, int from) {
        for (int i = from; i < parts.size(); i++) {
            try {
                parts.get(i).close();
            } catch (RuntimeException ignored) {
            }
        }
    }

    private Pending<List<Message>> registerRequest() {
        Pending<List<Message>> state = register(PendingKind.REQUEST, null,
            List.of(), 0);
        startRuntimeOwner();
        return state;
    }

    private <T> Pending<T> register(PendingKind kind, RoutingId target,
                                    List<Message> retained) {
        return register(kind, target, retained, 0);
    }

    private <T> Pending<T> register(PendingKind kind, RoutingId target,
                                    List<Message> retained,
                                    int requestTimeoutMs) {
        synchronized (ownerLock) {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            long token = nextContextTokenLocked();
            Pending<T> state = new Pending<>(token, kind, target, retained,
                requestTimeoutMs);
            pending.put(token, state);
            return state;
        }
    }

    private long nextContextToken() {
        synchronized (ownerLock) {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            return nextContextTokenLocked();
        }
    }

    private long nextContextTokenLocked() {
        long token;
        do {
            token = NEXT_CONTEXT.getAndIncrement();
        } while (token == 0L || pending.containsKey(token));
        return token;
    }

    private Pending<Void> registerAfterAttempt(long token, RoutingId target,
                                               List<Message> retained,
                                               PendingKind kind) {
        synchronized (ownerLock) {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            Pending<Void> state = new Pending<>(token, kind, target, retained,
                0);
            Pending<?> previous = pending.putIfAbsent(token, state);
            if (previous != null) {
                throw new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
            }
            return state;
        }
    }

    private Pending<List<Message>> registerRequestAfterAttempt(
            long token, RoutingId target, List<Message> retained,
            int timeoutMs) {
        synchronized (ownerLock) {
            if (closed) {
                throw new IllegalStateException("socket is closed");
            }
            Pending<List<Message>> state = new Pending<>(token,
                PendingKind.REQUEST, target, retained, timeoutMs);
            Pending<?> previous = pending.putIfAbsent(token, state);
            if (previous != null) {
                throw new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR);
            }
            return state;
        }
    }

    int drain(List<Runnable> settlements) {
        drainLock.lock();
        try {
            return drainWithNativeGate(settlements);
        } finally {
            signalDrainProgress();
            drainLock.unlock();
        }
    }

    int drainFromRuntime() {
        try {
            drainLock.lockInterruptibly();
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            return -1;
        }
        try {
            synchronized (ownerLock) {
                if (closed || publicOwner != null) {
                    return -1;
                }
            }
            return drainWithNativeGate(null);
        } finally {
            signalDrainProgress();
            drainLock.unlock();
        }
    }

    private int drainWithNativeGate(List<Runnable> settlements) {
        ReentrantReadWriteLock.ReadLock read = nativeCallGate.readLock();
        read.lock();
        try {
            if (closed) {
                return 0;
            }
            return drainLocked(settlements);
        } finally {
            read.unlock();
        }
    }

    private int drainLocked(List<Runnable> settlements) {
        int progress = 0;
        MemorySegment completion = NATIVE_SCRATCH.get().completion;
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
                throw new ZlinkRecvException(RecvResult.fromValue(rc),
                    Native.errno());
            }
            progress++;
            Pending<?> state = null;
            Object result = null;
            try {
                MemorySegment context = completion.get(
                    ValueLayout.ADDRESS,
                    NativeLayouts.COMPLETION_CONTEXT_OFFSET);
                state = pending.get(context.address());
                if (state != null) {
                    result = readResult(completion,
                        state.expectsRequestCompletion());
                }
            } finally {
                Native.completionClose(completion);
                CLOSED_COMPLETIONS.incrementAndGet();
            }
            if (state != null) {
                state.capture(result);
                if (settlements != null && state.terminalDispatched()) {
                    settlements.add(state::awaitSettlement);
                }
            }
        }
        return progress;
    }

    private Object readResult(MemorySegment completion, boolean request) {
        int kindValue = completion.get(ValueLayout.JAVA_INT,
            NativeLayouts.COMPLETION_KIND_OFFSET);
        if (request) {
            long completionId = completion.get(ValueLayout.JAVA_LONG,
                NativeLayouts.COMPLETION_ID_OFFSET);
            Object outcome;
            if (kindValue != CompletionKind.REQUEST.value()) {
                outcome = new ZlinkRequestException(
                    RequestResult.PROTOCOL_ERROR);
            } else {
                RequestResult result = RequestResult.fromValue(completion.get(
                    ValueLayout.JAVA_INT,
                    NativeLayouts.COMPLETION_REQUEST_RESULT_OFFSET));
                if (result != RequestResult.OK) {
                    outcome = new ZlinkRequestException(result);
                } else {
                    MemorySegment parts = completion.get(ValueLayout.ADDRESS,
                        NativeLayouts.COMPLETION_REPLY_PARTS_OFFSET);
                    long count = completion.get(ValueLayout.JAVA_LONG,
                        NativeLayouts.COMPLETION_REPLY_COUNT_OFFSET);
                    Message[] replies = InternalAccess
                        .messageFromOwnedMessageVectorShared(parts, count);
                    outcome = List.copyOf(Arrays.asList(replies));
                }
            }
            return new RequestCompletion(completionId, outcome);
        }

        RoutingId peer = NativeRoutingIds.read(completion.asSlice(
            NativeLayouts.COMPLETION_PEER_RID_OFFSET,
            NativeLayouts.ROUTING_ID_LAYOUT.byteSize()));
        return new WritableCompletion(kindValue,
            completion.get(ValueLayout.JAVA_LONG,
                NativeLayouts.COMPLETION_ID_OFFSET),
            completion.get(ValueLayout.ADDRESS,
                NativeLayouts.COMPLETION_CONTEXT_OFFSET).address(),
            peer,
            completion.get(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_SEND_RESULT_OFFSET),
            completion.get(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_SEND_ERRNO_OFFSET));
    }

    MemorySegment handle() {
        return socket.handle();
    }

    boolean transferToPublic(Object claimant) {
        Objects.requireNonNull(claimant, "claimant");
        drainLock.lock();
        try {
            synchronized (ownerLock) {
                if (publicOwner == claimant)
                    return false;
                if (publicOwner != null)
                    throw new IllegalStateException(
                        "completion queue already has a public poller owner");
                publicOwner = claimant;
                ownerLock.notifyAll();
            }
        } finally {
            drainLock.unlock();
        }
        try {
            runtime.unregister(this);
        } catch (RuntimeException | Error failure) {
            synchronized (ownerLock) {
                if (publicOwner == claimant)
                    publicOwner = null;
                ownerLock.notifyAll();
            }
            startRuntimeOwner();
            throw failure;
        }
        return true;
    }

    void releasePublic(Object claimant) {
        Objects.requireNonNull(claimant, "claimant");
        drainLock.lock();
        try {
            synchronized (ownerLock) {
                if (publicOwner != claimant)
                    return;
                publicOwner = null;
                ownerLock.notifyAll();
            }
        } finally {
            drainLock.unlock();
        }
        startRuntimeOwner();
    }

    private void startRuntimeOwner() {
        try {
            synchronized (ownerLock) {
                if (closed || publicOwner != null || pending.isEmpty())
                    return;
                runtime.register(this);
            }
        } catch (RuntimeException | Error failure) {
            rejectRuntimeStates(failure);
            throw failure;
        }
    }

    void rejectRuntimeStates(Throwable failure) {
        drainLock.lock();
        try {
            synchronized (ownerLock) {
                if (closed || publicOwner != null)
                    return;
            }
            for (Pending<?> state : new ArrayList<>(pending.values()))
                state.reject(failure);
        } finally {
            drainLock.unlock();
        }
    }

    private void retrySend(Pending<?> untyped) {
        @SuppressWarnings("unchecked")
        Pending<Void> state = (Pending<Void>) untyped;
        SubmitAttempt attempt;
        try {
            attempt = submitPartsAttempt(state.target, state.retained,
                SendFlags.DONT_WAIT.value(), 0, state.context(), true, false,
                0L);
        } catch (RuntimeException | Error failure) {
            state.reject(failure);
            return;
        }

        if (attempt.result() == SubmitResult.OK) {
            if (attempt.completionId() != 0L) {
                state.reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR));
                return;
            }
            state.completeSendAsync();
            return;
        }
        if (isWritableWait(attempt)) {
            state.armWritable(attempt.completionId());
        } else {
            state.reject(state.targetRemoved()
                ? new ZlinkSubmitException(SubmitResult.NOT_FOUND)
                : submitFailure(attempt));
        }
    }

    private void retryRequest(Pending<?> untyped) {
        @SuppressWarnings("unchecked")
        Pending<List<Message>> state =
            (Pending<List<Message>>) untyped;
        SubmitAttempt attempt;
        try {
            attempt = submitPartsAttempt(state.target, state.retained,
                SendFlags.DONT_WAIT.value(), state.requestTimeoutMs,
                state.context(), true, true, 0L);
        } catch (RuntimeException | Error failure) {
            state.reject(failure);
            return;
        }

        if (attempt.result() == SubmitResult.OK) {
            if (attempt.completionId() == 0L) {
                state.reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR));
                return;
            }
            state.publishRequest(attempt.completionId());
            state.releaseRetained();
            return;
        }
        if (isWritableWait(attempt)) {
            state.armWritable(attempt.completionId());
        } else {
            state.reject(state.targetRemoved()
                ? new ZlinkRequestException(RequestResult.NOT_FOUND)
                : submitFailure(attempt));
        }
    }

    private void rejectPending(Throwable failure) {
        for (Pending<?> state : pending.values()) {
            state.reject(failure);
        }
    }

    void rejectPendingClosed() {
        for (Pending<?> state : pending.values()) {
            state.rejectClosed();
        }
    }

    private void signalDrainProgress() {
        synchronized (ownerLock) {
            ownerLock.notifyAll();
        }
    }

    @Override
    public void close() {
        synchronized (ownerLock) {
            if (finalized)
                return;
            finalized = true;
            closed = true;
            publicOwner = this;
            ownerLock.notifyAll();
        }
        runtime.unregister(this);
        rejectPendingClosed();
        pending.clear();
    }

    private static boolean isWritableWait(SubmitAttempt attempt) {
        return attempt.result() == SubmitResult.BACKPRESSURED
            && NativeSubmitErrors.isBackpressured(attempt.errno())
            && attempt.completionId() != 0L;
    }

    private static void requireSuccess(SubmitAttempt attempt) {
        if (attempt.result() != SubmitResult.OK) {
            throw submitFailure(attempt);
        }
    }

    private static void requireRequestSuccess(SubmitAttempt attempt) {
        if (attempt.result() != SubmitResult.OK) {
            throw new ZlinkSubmitException(attempt.result(), attempt.errno());
        }
    }

    private static ZlinkSubmitException submitFailure(
            SubmitAttempt attempt) {
        if (attempt.result() == SubmitResult.BACKPRESSURED
                && NativeSubmitErrors.isBackpressured(attempt.errno())
                && attempt.completionId() == 0L) {
            return new ZlinkSubmitException(SubmitResult.INTERNAL_ERROR,
                attempt.errno());
        }
        return new ZlinkSubmitException(attempt.result(), attempt.errno());
    }

    private static ZlinkSubmitException terminalSendFailure(int errno) {
        if (errno != 0) {
            ZlinkSubmitException mapped =
                NativeSubmitErrors.submitExceptionOrNull(errno);
            if (mapped != null) {
                return mapped;
            }
        }
        return new ZlinkSubmitException(SubmitResult.NOT_ADMITTED, errno);
    }

    private static ZlinkRequestException terminalRequestFailure(int errno) {
        return (ZlinkRequestException) ZlinkException.fromErrno(
            ErrorCategory.REQUEST, errno);
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

    private enum PendingKind {
        REQUEST,
        RETRY_SEND,
        DISCARD_WRITABLE
    }

    private record SubmitAttempt(SubmitResult result, int errno,
                                 long completionId) {
    }

    record NoWaitAttempt(int result, int errno) {
    }

    @FunctionalInterface
    interface NoWaitSubmitter {
        int submit(MemorySegment userContext,
                   MemorySegment completionIdOut);
    }

    @FunctionalInterface
    interface NativeSendAction<T> {
        T run();
    }

    private record WritableCompletion(int kind, long completionId,
                                      long context, RoutingId peer,
                                      int sendResult, int terminalErrno) {
    }

    private record RequestCompletion(long completionId, Object outcome) {
    }

    private final class Pending<T> {
        private final long token;
        private final PendingKind kind;
        private final RoutingId target;
        private List<Message> retained;
        private final int requestTimeoutMs;
        private final CompletableFuture<T> future = new CompletableFuture<>();
        private boolean published;
        private boolean captured;
        private boolean dispatched;
        private long completionId;
        private Object result;
        private boolean requestAdmitted;
        private boolean targetRemoved;

        Pending(long token, PendingKind kind, RoutingId target,
                List<Message> retained, int requestTimeoutMs) {
            this.token = token;
            this.kind = kind;
            this.target = target;
            this.retained = retained;
            this.requestTimeoutMs = requestTimeoutMs;
            this.requestAdmitted = kind == PendingKind.REQUEST
                && retained.isEmpty();
        }

        MemorySegment context() {
            return MemorySegment.ofAddress(token);
        }

        void publishRequest(long id) {
            boolean ready;
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                completionId = id;
                published = true;
                requestAdmitted = true;
                ready = captured;
            }
            if (ready) {
                settleCapturedRequest();
            }
        }

        void armWritable(long id) {
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                completionId = id;
                published = true;
                if (kind == PendingKind.REQUEST)
                    requestAdmitted = false;
            }
            signalDrainProgress();
        }

        synchronized void markTargetRemoved() {
            targetRemoved = true;
        }

        synchronized boolean targetRemoved() {
            return targetRemoved;
        }

        void capture(Object value) {
            if (value instanceof RequestCompletion) {
                captureRequest(value);
            } else {
                captureWritable(value);
            }
        }

        private void captureRequest(Object value) {
            boolean ready;
            synchronized (this) {
                if (dispatched) {
                    closeCapturedValue(value);
                    return;
                }
                captured = true;
                result = value;
                ready = published;
            }
            if (ready) {
                settleCapturedRequest();
            }
        }

        private void settleCapturedRequest() {
            Object capturedResult;
            long expectedId;
            synchronized (this) {
                if (dispatched || !published || !captured) {
                    return;
                }
                capturedResult = result;
                expectedId = completionId;
            }
            if (!(capturedResult instanceof RequestCompletion completion)) {
                settle(null, new ZlinkRequestException(
                    RequestResult.PROTOCOL_ERROR), false);
                return;
            }
            if (completion.completionId() != expectedId) {
                closeCapturedValue(completion.outcome());
                settle(null, new ZlinkRequestException(
                    RequestResult.PROTOCOL_ERROR), false);
                return;
            }
            capturedResult = completion.outcome();
            if (capturedResult instanceof Throwable failure) {
                settle(null, failure, false);
            } else {
                settle(capturedResult, null, false);
            }
        }

        private void captureWritable(Object value) {
            if (!(value instanceof WritableCompletion completion)) {
                reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR));
                return;
            }

            Throwable failure = null;
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                if (completion.kind() != CompletionKind.WRITABLE.value()
                        || completion.completionId() != completionId
                        || completion.context() != token
                        || !Objects.equals(completion.peer(), target)) {
                    failure = new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                } else if (completion.sendResult() == SEND_TERMINAL) {
                    if (targetRemoved) {
                        failure = kind == PendingKind.REQUEST
                            ? new ZlinkRequestException(
                                RequestResult.NOT_FOUND)
                            : new ZlinkSubmitException(
                                SubmitResult.NOT_FOUND);
                    } else {
                        failure = kind == PendingKind.REQUEST
                            ? terminalRequestFailure(
                                completion.terminalErrno())
                            : terminalSendFailure(
                                completion.terminalErrno());
                    }
                } else if (completion.sendResult() != SEND_ADMITTED) {
                    failure = new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR,
                        completion.terminalErrno());
                }
            }
            if (failure != null) {
                reject(failure);
            } else if (kind == PendingKind.DISCARD_WRITABLE) {
                completeDiscardInline();
            } else if (kind == PendingKind.REQUEST) {
                retryRequest(this);
            } else {
                retrySend(this);
            }
        }

        synchronized boolean expectsRequestCompletion() {
            return kind == PendingKind.REQUEST && requestAdmitted;
        }

        void releaseRetained() {
            List<Message> released;
            synchronized (this) {
                released = retained;
                retained = List.of();
            }
            closeParts(released);
        }

        void completeSendInline() {
            settle(null, null, true);
        }

        void completeSendAsync() {
            settle(null, null, false);
        }

        void completeDiscardInline() {
            settle(null, null, true);
        }

        void reject(Throwable failure) {
            settle(null, Objects.requireNonNull(failure, "failure"), false);
        }

        private void settle(Object value, Throwable failure, boolean inline) {
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                dispatched = true;
            }
            pending.remove(token, this);
            releaseRetained();
            signalDrainProgress();

            Runnable completion = () -> {
                try {
                    if (failure != null) {
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
            };
            if (inline) {
                completion.run();
            } else {
                lane.dispatch(completion);
            }
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

        synchronized boolean terminalDispatched() {
            return dispatched;
        }

        synchronized boolean awaitingWritable() {
            return published && !dispatched
                && (kind != PendingKind.REQUEST || !requestAdmitted);
        }

        void rejectClosed() {
            if (kind == PendingKind.REQUEST) {
                reject(new ZlinkRequestException(RequestResult.TERMINATED,
                    NativeErrno.ESHUTDOWN));
            } else {
                reject(new ZlinkSubmitException(SubmitResult.TERMINATED,
                    NativeErrno.ESHUTDOWN));
            }
        }
    }

    private static void closeCapturedValue(Object value) {
        if (value instanceof RequestCompletion completion) {
            value = completion.outcome();
        }
        if (!(value instanceof List<?> values)) {
            return;
        }
        for (Object element : values) {
            if (element instanceof Message message) {
                try {
                    message.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }
}
