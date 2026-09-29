/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.locks.ReentrantLock;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkRecvException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.RequestSubmission;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.CompletionKind;
import systems.zlink.contracts.sockets.RecvResult;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.sockets.SocketOption;
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
    private static final int SEND_NOT_FOUND = 801;
    private static final int SEND_NOT_CONNECTED = 802;
    private static final int RECV_DONT_WAIT = 1;
    private static final CompletionStage<Void> COMPLETED_ADMISSION =
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
    private final ConcurrentHashMap<Long, Pending<?>> pending =
        new ConcurrentHashMap<>();
    private final Object ownerLock = new Object();
    private final HashMap<Long, WritableCompletion> earlyWritable =
        new HashMap<>();
    private long[] unpublishedSends = new long[4];
    private int unpublishedSendCount;
    private final ReentrantLock drainLock = new ReentrantLock();
    private final ReentrantLock inlineDrainLock = new ReentrantLock();
    private final ConcurrentLinkedQueue<Pending<?>> retries =
        new ConcurrentLinkedQueue<>();
    private PublicSettlement publicSettlementHead;
    private PublicSettlement publicSettlementTail;
    private volatile boolean closed;
    private boolean finalized;
    private Object publicOwner;

    CompletionOwner(NativeSocketRuntime socket) {
        this.socket = Objects.requireNonNull(socket, "socket");
    }

    SendSubmission submitSend(RoutingId target, List<Message> parts) {
        boolean hasPublicOwner = hasPublicOwner();
        long token = nextSendContextToken();
        SubmitAttempt attempt;
        List<Message> retained;
        try {
            attempt = submitPartsAttempt(target, parts,
                SendFlags.DONT_WAIT.value(), 0,
                MemorySegment.ofAddress(token), true, false,
                0L);
            if (attempt.result() == SubmitResult.OK) {
                if (attempt.completionId() != 0L) {
                    throw new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                }
                discardUnpublishedSend(token);
                closeParts(parts);
                return new SendSubmissionValue(SubmitResult.OK,
                    COMPLETED_ADMISSION);
            }
            if (!isWritableWait(attempt)) {
                throw submitFailure(attempt);
            }
            if (!hasPublicOwner) {
                throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
            }
            retained = retainParts(parts);
        } catch (RuntimeException | Error failure) {
            discardUnpublishedSend(token);
            throw failure;
        }
        Pending<Void> state;
        boolean rejectClosed;
        synchronized (ownerLock) {
            removeUnpublishedSendLocked(token);
            state = new Pending<>(token, PendingKind.RETRY_SEND, target,
                retained, 0);
            state.armWritable(attempt.completionId());
            rejectClosed = closed;
            if (!rejectClosed)
                pending.put(token, state);
            ownerLock.notifyAll();
        }
        if (rejectClosed)
            state.rejectClosed();
        closeParts(parts);
        return new SendSubmissionValue(SubmitResult.BACKPRESSURED,
            state.admitted);
    }

    void submitSendBlocking(RoutingId target, List<Message> parts) {
        SubmitAttempt attempt = submitPartsAttempt(target, parts,
            SendFlags.NONE.value(), 0, MemorySegment.NULL, false, false, 0L);
        requireSuccess(attempt);
        closeParts(parts);
    }

    RequestSubmission submitRequest(RoutingId target, List<Message> parts,
                                    Duration timeout) {
        ensurePublicOwner();
        int timeoutMs = timeoutMillis(timeout);
        Pending<List<Message>> state = register(PendingKind.REQUEST, target,
            List.of(), timeoutMs);
        SubmitAttempt attempt;
        List<Message> retained;
        try {
            attempt = submitPartsAttempt(target, parts,
                SendFlags.DONT_WAIT.value(), timeoutMs, state.context(), true,
                true, 0L);
            if (attempt.result() == SubmitResult.OK) {
                if (attempt.completionId() == 0L) {
                    throw new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                }
                state.publishRequest(attempt.completionId());
                closeParts(parts);
                return new RequestSubmissionValue(SubmitResult.OK,
                    state.admitted, state.future);
            }
            if (!isWritableWait(attempt)) {
                throw submitFailure(attempt);
            }
            retained = retainParts(parts);
        } catch (RuntimeException | Error failure) {
            state.abandonSend();
            throw failure;
        }
        if (!state.retain(retained)) {
            closeParts(retained);
        }
        state.armWritable(attempt.completionId());
        closeParts(parts);
        return new RequestSubmissionValue(SubmitResult.BACKPRESSURED,
            state.admitted, state.future);
    }

    List<Message> submitRequestBlocking(RoutingId target,
                                        List<Message> parts,
                                        Duration timeout) {
        try {
            Pending<List<Message>> state = submitRequestWithFlags(target, parts,
                timeout, SendFlags.NONE.value());
            drainInline(state);
            return state.future.join();
        } catch (java.util.concurrent.CompletionException failure) {
            if (failure.getCause() instanceof RuntimeException runtime) {
                throw runtime;
            }
            throw failure;
        }
    }

    private Pending<List<Message>> submitRequestWithFlags(
            RoutingId target, List<Message> parts, Duration timeout, int flags) {
        Pending<List<Message>> state = null;
        try {
            state = registerRequest();
            SubmitAttempt attempt = submitPartsAttempt(target, parts,
                flags, timeoutMillis(timeout), state.context(), true,
                true, 0L);
            requireSuccess(attempt);
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
        return state;
    }

    void submitReply(RoutingId target, long token, List<Message> parts) {
        SubmitAttempt attempt = submitPartsAttempt(target, parts,
            SendFlags.NONE.value(), 0, MemorySegment.NULL, false, false, token);
        requireSuccess(attempt);
        closeParts(parts);
    }

    <T> T withNativeCall(NativeSendAction<T> action) {
        Objects.requireNonNull(action, "action");
        if (closed) {
            throw new IllegalStateException("socket is closed");
        }
        return action.run();
    }

    /**
     * Submits a caller-owned DONT_WAIT send. A WRITABLE token needs no
     * continuation: Core ends it and the drain closes the unmatched record.
     */
    NoWaitAttempt trackNoWaitSend(NoWaitSubmitter submitter) {
        Objects.requireNonNull(submitter, "submitter");
        long token = nextContextToken();
        return withNativeCall(() -> {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment idOut = arena.allocate(ValueLayout.JAVA_LONG);
                int result = submitter.submit(MemorySegment.ofAddress(token),
                    idOut);
                int errno = result == SubmitResult.OK.value()
                    ? 0 : Native.errno();
                SubmitResult submitted = SubmitResult.fromValue(result);
                if (submitted == SubmitResult.OK
                        && idOut.get(ValueLayout.JAVA_LONG, 0) != 0L) {
                    throw new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                }
                return new NoWaitAttempt(submitted.value(), errno);
            }
        });
    }

    private SubmitAttempt submitPartsAttempt(
            RoutingId target, List<Message> originals, int flags,
            int timeoutMs, MemorySegment userContext, boolean completion,
            boolean request, long replyToken) {
        return withNativeCall(() -> submitPartsAttemptLocked(target, originals,
            flags, timeoutMs, userContext, completion, request, replyToken));
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
        boolean submitted = false;
        try {
            for (; initialized < originals.size(); initialized++) {
                InternalAccess.messageCopyTo(originals.get(initialized),
                    nativeParts.asSlice(partSize * initialized, partSize));
            }
            submitted = true;
            int rc;
            if (!request && replyToken == 0L) {
                rc = target == null
                    ? Native.send(socket.handle(), nativeParts,
                        originals.size(), flags, userContext, idOut)
                    : Native.sendRid(socket.handle(), nativeTarget,
                        nativeParts, originals.size(), flags, userContext,
                        idOut);
            } else if (request) {
                rc = Native.request(socket.handle(), nativeTarget,
                    nativeParts, originals.size(), flags, timeoutMs,
                    userContext, idOut);
            } else {
                rc = Native.reply(socket.handle(), nativeTarget,
                    replyToken, nativeParts, originals.size());
            }
            if (rc != SubmitResult.OK.value()) {
                int errno = Native.errno();
                long id = completion
                    ? idOut.get(ValueLayout.JAVA_LONG, 0) : 0L;
                return new SubmitAttempt(SubmitResult.fromValue(rc), errno,
                    id);
            }
            long id = completion
                ? idOut.get(ValueLayout.JAVA_LONG, 0) : 0L;
            return new SubmitAttempt(SubmitResult.OK, 0, id);
        } finally {
            for (int index = submitted ? initialized : 0;
                 index < initialized; index++) {
                NativeMessage.messageClose(nativeParts.asSlice(
                    partSize * index, partSize));
            }
        }
    }

    int closeNativeSocket() {
        int rc = Native.close(socket.handle());
        if (rc == systems.zlink.contracts.errors.CloseResult.OK.value()) {
            synchronized (ownerLock) {
                closed = true;
            }
        }
        return rc;
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
        return register(PendingKind.REQUEST, null, List.of(), 0);
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

    private long nextSendContextToken() {
        synchronized (ownerLock) {
            if (closed)
                throw new IllegalStateException("socket is closed");
            long token = nextContextTokenLocked();
            if (unpublishedSendCount == unpublishedSends.length)
                unpublishedSends = Arrays.copyOf(unpublishedSends,
                    unpublishedSends.length * 2);
            unpublishedSends[unpublishedSendCount++] = token;
            return token;
        }
    }

    private void discardUnpublishedSend(long token) {
        synchronized (ownerLock) {
            removeUnpublishedSendLocked(token);
            if (earlyWritable.remove(token) != null)
                ownerLock.notifyAll();
        }
    }

    private void removeUnpublishedSendLocked(long token) {
        for (int i = 0; i < unpublishedSendCount; i++) {
            if (unpublishedSends[i] == token) {
                unpublishedSends[i] = unpublishedSends[--unpublishedSendCount];
                return;
            }
        }
    }

    private boolean unpublishedSendLocked(long token) {
        for (int i = 0; i < unpublishedSendCount; i++) {
            if (unpublishedSends[i] == token)
                return true;
        }
        return false;
    }

    private long nextContextTokenLocked() {
        long token;
        do {
            token = NEXT_CONTEXT.getAndIncrement();
        } while (token == 0L || pending.containsKey(token));
        return token;
    }

    int drain() {
        PublicSettlement settlements = null;
        try {
            drainLock.lock();
            try {
                return drainLocked(true);
            } finally {
                settlements = detachPublicSettlements();
                drainLock.unlock();
            }
        } finally {
            completePublicSettlements(settlements);
        }
    }

    private void enqueuePublicSettlement(Runnable completion) {
        PublicSettlement state = new PublicSettlement(completion);
        if (publicSettlementTail == null) {
            publicSettlementHead = state;
        } else {
            publicSettlementTail.next = state;
        }
        publicSettlementTail = state;
    }

    private PublicSettlement detachPublicSettlements() {
        PublicSettlement head = publicSettlementHead;
        publicSettlementHead = null;
        publicSettlementTail = null;
        return head;
    }

    private static void completePublicSettlements(PublicSettlement state) {
        while (state != null) {
            PublicSettlement next = state.next;
            state.completion.run();
            state = next;
        }
    }

    private void drainInline(Pending<List<Message>> target) {
        inlineDrainLock.lock();
        try {
            if (hasPublicOwner())
                return;
            while (!target.future.isDone()) {
                PublicSettlement settlements = null;
                try {
                    drainLock.lock();
                    try {
                        receiveOneBlocking();
                        drainLocked(true);
                    } catch (RuntimeException | Error failure) {
                        target.reject(failure, true);
                        throw failure;
                    } finally {
                        settlements = detachPublicSettlements();
                        drainLock.unlock();
                    }
                } finally {
                    completePublicSettlements(settlements);
                }
            }
        } finally {
            inlineDrainLock.unlock();
        }
    }

    private void receiveOneBlocking() {
        if (closed)
            throw new IllegalStateException("socket is closed");
        MemorySegment completion = NATIVE_SCRATCH.get().completion;
        while (true) {
            completion.fill((byte) 0);
            completion.set(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_STRUCT_SIZE_OFFSET,
                (int) NativeLayouts.COMPLETION_LAYOUT.byteSize());
            int rc = Native.completionRecv(socket.handle(), completion, 0);
            if (rc == RecvResult.NO_DATA.value())
                continue;
            if (rc != RecvResult.OK.value()) {
                throw new ZlinkRecvException(RecvResult.fromValue(rc),
                    Native.errno());
            }
            capture(completion, true);
            return;
        }
    }

    private int drainLocked(boolean settleInline) {
        int progress = 0;
        MemorySegment completion = NATIVE_SCRATCH.get().completion;
        while (!closed) {
            completion.fill((byte) 0);
            completion.set(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_STRUCT_SIZE_OFFSET,
                (int) NativeLayouts.COMPLETION_LAYOUT.byteSize());
            int rc = Native.completionRecv(socket.handle(), completion,
                RECV_DONT_WAIT);
            if (rc == RecvResult.NO_DATA.value()) {
                Pending<?> state;
                while (!closed && (state = retries.poll()) != null) {
                    if (!state.awaitingWritable())
                        continue;
                    if (state.kind == PendingKind.REQUEST)
                        retryRequest(state, settleInline);
                    else
                        retrySend(state, settleInline);
                }
                break;
            }
            if (rc == RecvResult.BUSY.value()) {
                break;
            }
            if (rc != RecvResult.OK.value()) {
                throw new ZlinkRecvException(RecvResult.fromValue(rc),
                    Native.errno());
            }
            if (capture(completion, settleInline))
                progress++;
        }
        return progress;
    }

    private boolean capture(MemorySegment completion, boolean settleInline) {
        Pending<?> state = null;
        Object result = null;
        boolean storedEarly = false;
        long token;
        try {
            MemorySegment context = completion.get(ValueLayout.ADDRESS,
                NativeLayouts.COMPLETION_CONTEXT_OFFSET);
            token = context.address();
            synchronized (ownerLock) {
                state = pending.get(token);
                if (state == null && unpublishedSendLocked(token)) {
                    WritableCompletion record = (WritableCompletion)
                        readResult(completion, false);
                    earlyWritable.put(token, record);
                    storedEarly = true;
                } else if (state != null
                           && state.kind != PendingKind.REQUEST) {
                    result = readResult(completion, false);
                }
            }
            if (state != null && state.kind == PendingKind.REQUEST
                    && state.awaitPublication())
                result = readResult(completion,
                    state.expectsRequestCompletion());
            else if (state != null && result == null)
                state = null;
        } finally {
            Native.completionClose(completion);
            CLOSED_COMPLETIONS.incrementAndGet();
        }
        if (storedEarly) {
            boolean interrupted = false;
            synchronized (ownerLock) {
                while (unpublishedSendLocked(token) && !closed) {
                    try {
                        ownerLock.wait();
                    } catch (InterruptedException failure) {
                        interrupted = true;
                    }
                }
                result = earlyWritable.remove(token);
                if (!closed && result != null)
                    state = pending.get(token);
            }
            if (interrupted)
                Thread.currentThread().interrupt();
        }
        if (state != null)
            state.capture(result, settleInline);
        return true;
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

        return new WritableCompletion(kindValue,
            completion.get(ValueLayout.JAVA_LONG,
                NativeLayouts.COMPLETION_ID_OFFSET),
            completion.get(ValueLayout.ADDRESS,
                NativeLayouts.COMPLETION_CONTEXT_OFFSET).address(),
            completion.get(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_SEND_RESULT_OFFSET),
            completion.get(ValueLayout.JAVA_INT,
                NativeLayouts.COMPLETION_SEND_ERRNO_OFFSET));
    }

    MemorySegment handle() {
        return socket.handle();
    }

    private boolean hasPublicOwner() {
        synchronized (ownerLock) {
            return publicOwner != null;
        }
    }

    private void ensurePublicOwner() {
        if (!hasPublicOwner())
            throw new ZlinkSubmitException(SubmitResult.INVALID_STATE);
    }

    boolean transferToPublic(Object claimant) {
        Objects.requireNonNull(claimant, "claimant");
        inlineDrainLock.lock();
        try {
            drainLock.lock();
            try {
                synchronized (ownerLock) {
                    if (closed)
                        throw new IllegalStateException("socket is closed");
                    if (publicOwner == claimant)
                        return false;
                    if (publicOwner != null)
                        throw new IllegalStateException(
                            "completion queue already has a public poller owner");
                    publicOwner = claimant;
                }
            } finally {
                drainLock.unlock();
            }
        } finally {
            inlineDrainLock.unlock();
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
            }
        } finally {
            drainLock.unlock();
        }
    }

    private void retrySend(Pending<?> untyped, boolean settleInline) {
        @SuppressWarnings("unchecked")
        Pending<Void> state = (Pending<Void>) untyped;
        if (!state.beginRetry()) {
            return;
        }
        SubmitAttempt attempt;
        try {
            attempt = submitPartsAttempt(state.target, state.retained,
                SendFlags.DONT_WAIT.value(), 0, state.context(), true, false,
                0L);
        } catch (RuntimeException | Error failure) {
            state.finishRetry(true);
            state.reject(failure, settleInline);
            return;
        }

        boolean failed = attempt.result() != SubmitResult.OK
            && !isWritableWait(attempt);
        if (state.finishRetry(failed)) {
            return;
        }

        if (attempt.result() == SubmitResult.OK) {
            if (attempt.completionId() != 0L) {
                state.reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR), settleInline);
                return;
            }
            state.completeSend(settleInline);
            return;
        }
        if (isWritableWait(attempt)) {
            state.armWritable(attempt.completionId());
        } else {
            state.reject(submitFailure(attempt), settleInline);
        }
    }

    private void retryRequest(Pending<?> untyped, boolean settleInline) {
        @SuppressWarnings("unchecked")
        Pending<List<Message>> state =
            (Pending<List<Message>>) untyped;
        if (!state.beginRetry()) {
            return;
        }
        SubmitAttempt attempt;
        try {
            attempt = submitPartsAttempt(state.target, state.retained,
                SendFlags.DONT_WAIT.value(), state.requestTimeoutMs,
                state.context(), true, true, 0L);
        } catch (RuntimeException | Error failure) {
            state.finishRetry(true);
            state.reject(failure, settleInline);
            return;
        }
        boolean failed = attempt.result() != SubmitResult.OK
            && !isWritableWait(attempt);
        if (state.finishRetry(failed)) {
            return;
        }

        if (attempt.result() == SubmitResult.OK) {
            if (attempt.completionId() == 0L) {
                state.reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR), settleInline);
                return;
            }
            state.publishRequest(attempt.completionId(), settleInline);
            state.releaseRetained();
            return;
        }
        if (isWritableWait(attempt)) {
            state.armWritable(attempt.completionId());
        } else {
            state.reject(submitFailure(attempt), settleInline);
        }
    }

    void rejectPendingClosed() {
        for (Pending<?> state : pending.values()) {
            state.rejectClosed();
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
        rejectPendingClosed();
        pending.clear();
        synchronized (ownerLock) {
            earlyWritable.clear();
        }
        retries.clear();
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

    private static ZlinkSubmitException submitFailure(
            SubmitAttempt attempt) {
        return new ZlinkSubmitException(attempt.result(), attempt.errno());
    }

    private static ZlinkSubmitException writableFailure(int result, int errno) {
        return new ZlinkSubmitException(
            result == SEND_NOT_FOUND ? SubmitResult.NOT_FOUND
                : SubmitResult.NOT_CONNECTED, errno);
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
        RETRY_SEND
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
                                      long context,
                                      int sendResult, int terminalErrno) {
    }

    private record RequestCompletion(long completionId, Object outcome) {
    }

    private record SendSubmissionValue(
            SubmitResult result,
            CompletionStage<Void> admitted) implements SendSubmission {
    }

    private record RequestSubmissionValue(
            SubmitResult result,
            CompletionStage<Void> admitted,
            CompletionStage<List<Message>> reply)
            implements RequestSubmission {
    }

    private static final class PublicSettlement {
        private final Runnable completion;
        private PublicSettlement next;

        private PublicSettlement(Runnable completion) {
            this.completion = completion;
        }
    }

    private final class Pending<T> {
        private final long token;
        private final PendingKind kind;
        private final RoutingId target;
        private List<Message> retained;
        private final int requestTimeoutMs;
        private final CompletableFuture<T> future = new CompletableFuture<>();
        private final CompletableFuture<Void> admitted =
            new CompletableFuture<>() {
                @Override
                public boolean cancel(boolean mayInterruptIfRunning) {
                    synchronized (Pending.this) {
                        if (kind == PendingKind.RETRY_SEND
                                && retryActive) {
                            cancelRequested = true;
                            return true;
                        }
                    }
                    boolean cancelled = super.cancel(mayInterruptIfRunning);
                    if (cancelled && kind == PendingKind.RETRY_SEND) {
                        abandonSend();
                    }
                    return cancelled;
                }
            };
        private boolean published;
        private boolean dispatched;
        private long completionId;
        private boolean requestAdmitted;
        private Object terminalValue;
        private Throwable terminalFailure;
        private boolean retryActive;
        private boolean cancelRequested;
        private boolean closeRequested;

        Pending(long token, PendingKind kind, RoutingId target,
                List<Message> retained, int requestTimeoutMs) {
            this.token = token;
            this.kind = kind;
            this.target = target;
            this.retained = retained;
            this.requestTimeoutMs = requestTimeoutMs;
        }

        MemorySegment context() {
            return MemorySegment.ofAddress(token);
        }

        void publishRequest(long id) {
            publishRequest(id, false);
        }

        void publishRequest(long id, boolean settleInline) {
            boolean completeAdmission;
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                completionId = id;
                published = true;
                completeAdmission = !requestAdmitted;
                requestAdmitted = true;
                notifyAll();
            }
            if (completeAdmission) {
                completeAdmission(settleInline);
            }
        }

        /** Keeps the message copies for the WRITABLE resubmission. */
        synchronized boolean retain(List<Message> parts) {
            if (dispatched) {
                return false;
            }
            retained = parts;
            return true;
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
                notifyAll();
            }
        }

        /**
         * Joins a record the drain read before the submit result was
         * published (async model 5). Only this entry's monitor is held, so
         * concurrent submits and other entries are not blocked.
         *
         * @return false when the entry ended first and the record is unmatched
         */
        boolean awaitPublication() {
            boolean interrupted = false;
            synchronized (this) {
                while (!published && !dispatched) {
                    try {
                        wait();
                    } catch (InterruptedException failure) {
                        interrupted = true;
                    }
                }
                if (interrupted) {
                    Thread.currentThread().interrupt();
                }
                return !dispatched;
            }
        }

        void capture(Object value, boolean settleInline) {
            if (value instanceof RequestCompletion) {
                captureRequest(value, settleInline);
            } else {
                captureWritable(value, settleInline);
            }
        }

        private void captureRequest(Object capturedResult,
                                    boolean settleInline) {
            long expectedId;
            synchronized (this) {
                if (dispatched) {
                    closeCapturedValue(capturedResult);
                    return;
                }
                expectedId = completionId;
            }
            RequestCompletion completion = (RequestCompletion) capturedResult;
            if (completion.completionId() != expectedId) {
                closeCapturedValue(completion.outcome());
                settle(null, new ZlinkRequestException(
                    RequestResult.PROTOCOL_ERROR), settleInline);
                return;
            }
            capturedResult = completion.outcome();
            if (capturedResult instanceof Throwable failure) {
                settle(null, failure, settleInline);
            } else {
                settle(capturedResult, null, settleInline);
            }
        }

        private void captureWritable(Object value, boolean settleInline) {
            if (!(value instanceof WritableCompletion completion)) {
                reject(new ZlinkSubmitException(
                    SubmitResult.INTERNAL_ERROR), settleInline);
                return;
            }

            Throwable failure = null;
            synchronized (this) {
                if (dispatched) {
                    return;
                }
                if (completion.kind() != CompletionKind.WRITABLE.value()
                        || completion.completionId() != completionId
                        || completion.context() != token) {
                    failure = new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR);
                } else if (completion.sendResult() == SEND_NOT_FOUND
                           || completion.sendResult() == SEND_NOT_CONNECTED) {
                    failure = writableFailure(completion.sendResult(),
                                              completion.terminalErrno());
                } else if (completion.sendResult() != SEND_ADMITTED) {
                    failure = new ZlinkSubmitException(
                        SubmitResult.INTERNAL_ERROR,
                        NativeErrno.EPROTO);
                }
            }
            if (failure != null) {
                reject(failure, settleInline);
            } else {
                // Core requires the current queue to reach NO_DATA before
                // a WRITABLE token can trigger another native submission.
                retries.add(this);
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

        void abandonSend() {
            if (prepareSettlement(null, null)) {
                completeFuture();
            }
        }

        synchronized boolean beginRetry() {
            if (closed || dispatched || (kind == PendingKind.RETRY_SEND
                    && admitted.isCancelled())) {
                return false;
            }
            retryActive = true;
            return true;
        }

        boolean finishRetry(boolean failed) {
            boolean cancel;
            boolean close;
            synchronized (this) {
                retryActive = false;
                cancel = cancelRequested && !failed;
                close = closeRequested;
                cancelRequested = false;
                closeRequested = false;
            }
            if (cancel) {
                admitted.cancel(false);
            } else if (close) {
                rejectClosed();
            }
            return cancel || close;
        }

        void completeSend(boolean inline) {
            settle(null, null, inline);
        }

        private void completeAdmission(boolean inline) {
            if (inline) {
                enqueuePublicSettlement(() -> admitted.complete(null));
            } else {
                admitted.complete(null);
            }
        }

        void reject(Throwable failure) {
            settle(null, Objects.requireNonNull(failure, "failure"), false);
        }

        void reject(Throwable failure, boolean inline) {
            settle(null, Objects.requireNonNull(failure, "failure"), inline);
        }

        private void settle(Object value, Throwable failure, boolean inline) {
            if (!prepareSettlement(value, failure)) {
                return;
            }
            if (inline) {
                enqueuePublicSettlement(this::completeFuture);
            } else {
                completeFuture();
            }
        }

        private boolean prepareSettlement(Object value, Throwable failure) {
            synchronized (this) {
                if (dispatched) {
                    return false;
                }
                dispatched = true;
                terminalValue = value;
                terminalFailure = failure;
                notifyAll();
            }
            pending.remove(token, this);
            releaseRetained();
            return true;
        }

        private void completeFuture() {
            try {
                if (terminalFailure != null) {
                    admitted.completeExceptionally(terminalFailure);
                    future.completeExceptionally(terminalFailure);
                } else {
                    admitted.complete(null);
                    @SuppressWarnings("unchecked")
                    T typed = (T) terminalValue;
                    future.complete(typed);
                }
            } finally {
                terminalValue = null;
                terminalFailure = null;
                synchronized (this) {
                    notifyAll();
                }
            }
        }

        synchronized boolean awaitingWritable() {
            return published && !dispatched
                && (kind != PendingKind.REQUEST || !requestAdmitted);
        }

        void rejectClosed() {
            synchronized (this) {
                if (retryActive) {
                    closeRequested = true;
                    return;
                }
            }
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
