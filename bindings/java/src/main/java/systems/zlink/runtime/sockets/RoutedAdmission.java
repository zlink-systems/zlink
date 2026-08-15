/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;
import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ErrorCategory;
import systems.zlink.contracts.errors.ZlinkException;
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.internal.NativeErrorCodes;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.MessagePartsBuffer;
import systems.zlink.runtime.nativeapi.Native;
import systems.zlink.runtime.nativeapi.NativeLayouts;
import systems.zlink.runtime.nativeapi.NativeRoutingIds;
import systems.zlink.runtime.nativeapi.RequestReplySupport;
import systems.zlink.runtime.nativeapi.RoutedRequestSupport;
import systems.zlink.runtime.nativeapi.RuntimeResources;

/**
 * Socket-owned admission for asynchronous DEALER/ROUTER send and request.
 *
 * <p>All mutable scheduling state is private to this aggregate. Native
 * readiness callbacks only copy the exact target key and schedule the pump;
 * they never submit. The pump runs one complete-record DONTWAIT part loop per
 * ready target outside {@link #lock}, preserving its internal queue without
 * coupling unrelated targets.
 */
final class RoutedAdmission {
    static final int ROUTED_WRITABLE = 1;
    static final int ROUTED_TERMINAL = 2;
    private static final int DONT_WAIT = 1;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final FunctionDescriptor READY_CALLBACK_DESCRIPTOR =
        FunctionDescriptor.ofVoid(ValueLayout.ADDRESS, ValueLayout.ADDRESS,
            ValueLayout.ADDRESS);
    private static final long NANOS_PER_MILLI = 1_000_000L;

    private final Object lock = new Object();
    private final NativeAccess nativeAccess;
    private final ReplyRegistry replies;
    private final Executor executor;
    private final ScheduledExecutorService deadlines;
    private final PartSnapshotter partSnapshotter;
    private final boolean ownsExecutor;
    private final MemorySegment readyCallbackSocketHandle;
    private final Map<Target, ArrayDeque<Operation<?>>> pendingByTarget =
        new HashMap<>();
    private final ArrayDeque<Target> readyTargets = new ArrayDeque<>();
    private final Set<Target> readySet = new HashSet<>();
    private final Map<Target, Long> wakeVersions = new HashMap<>();
    private final Set<Operation<?>> lifecycle =
        java.util.Collections.newSetFromMap(new IdentityHashMap<>());

    private Arena callbackArena;
    private boolean closing;
    private boolean closed;
    private boolean pumpScheduled;
    private boolean pumping;
    private int activeAttempts;
    private int activeNativeCallbacks;
    private final RoutedRequestSupport.CallbackLifecycle nativeCallbacks =
        new RoutedRequestSupport.CallbackLifecycle() {
            @Override
            public void enter() {
                enterNativeCallback();
            }

            @Override
            public void exit() {
                exitNativeCallback();
            }
        };

    RoutedAdmission(MemorySegment socketHandle, boolean dealer,
                    OutboundRecordAttemptGate attemptGate) {
        this(new PanamaNativeAccess(socketHandle, dealer, attemptGate),
            new NativeReplyRegistry(),
            RuntimeResources.daemonSingleThreadExecutor(
                "zlink-routed-admission"), SharedDeadlines.INSTANCE,
            RoutedAdmission::snapshotSharedParts, true, socketHandle);
    }

    RoutedAdmission(NativeAccess nativeAccess, ReplyRegistry replies,
                    Executor executor, ScheduledExecutorService deadlines) {
        this(nativeAccess, replies, executor, deadlines,
            PartSnapshot::borrowed, false, MemorySegment.NULL);
    }

    private RoutedAdmission(NativeAccess nativeAccess, ReplyRegistry replies,
                            Executor executor,
                            ScheduledExecutorService deadlines,
                            PartSnapshotter partSnapshotter,
                            boolean ownsExecutor,
                            MemorySegment readyCallbackSocketHandle) {
        this.nativeAccess = Objects.requireNonNull(nativeAccess,
            "nativeAccess");
        this.replies = Objects.requireNonNull(replies, "replies");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.deadlines = Objects.requireNonNull(deadlines, "deadlines");
        this.partSnapshotter = Objects.requireNonNull(partSnapshotter,
            "partSnapshotter");
        this.ownsExecutor = ownsExecutor;
        this.readyCallbackSocketHandle = Objects.requireNonNull(
            readyCallbackSocketHandle, "readyCallbackSocketHandle");
    }

    CompletionStage<Void> send(RoutingId selector, List<Message> parts,
                               int sendTimeoutMs) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty())
            throw new IllegalArgumentException("parts must not be empty");
        ensureReadyCallbackInstalled();
        long started = System.nanoTime();
        long deadline = sendTimeoutMs < 0
            ? Long.MAX_VALUE
            : saturatingDeadline(started, sendTimeoutMs);
        OperationFuture<Void> future = new OperationFuture<>();
        List<Message> sources = new ArrayList<>(parts);
        PartSnapshot snapshot;
        try {
            snapshot = partSnapshotter.snapshot(sources);
        } catch (Throwable error) {
            future.completeExceptionally(error);
            return future;
        }
        Operation<Void> operation = Operation.send(future,
            sources, snapshot, deadline, sendTimeoutMs == 0);
        begin(operation, selector, null);
        return future;
    }

    CompletionStage<List<Message>> request(RoutingId selector,
                                            List<Message> parts,
                                            Duration timeout) {
        return request(selector, null, parts, timeout);
    }

    CompletionStage<List<Message>> request(Target exactTarget,
                                            List<Message> parts,
                                            Duration timeout) {
        Objects.requireNonNull(exactTarget, "exactTarget");
        return request(null, exactTarget, parts, timeout);
    }

    private CompletionStage<List<Message>> request(
            RoutingId selector, Target exactTarget, List<Message> parts,
            Duration timeout) {
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty())
            throw new IllegalArgumentException("parts must not be empty");
        ensureReadyCallbackInstalled();
        long timeoutMs = RequestReplySupport.timeoutMillis(timeout);
        long deadline = saturatingDeadline(System.nanoTime(), timeoutMs);
        OperationFuture<List<Message>> future = new OperationFuture<>();
        List<Message> sources = new ArrayList<>(parts);
        PartSnapshot snapshot;
        try {
            snapshot = partSnapshotter.snapshot(sources);
        } catch (Throwable error) {
            future.completeExceptionally(error);
            return future;
        }
        long requestId = 0L;
        Operation<List<Message>> operation;
        try {
            requestId = replies.nextRequestId();
            operation = Operation.request(future, sources, snapshot, deadline,
                requestId);
            replies.register(requestId, future, nativeCallbacks);
        } catch (Throwable error) {
            if (requestId != 0L) {
                try {
                    replies.remove(requestId);
                } catch (Throwable ignored) {
                }
            }
            snapshot.release();
            future.completeExceptionally(error);
            return future;
        }
        begin(operation, selector, exactTarget);
        return future;
    }

    private void begin(Operation<?> operation, RoutingId selector,
                       Target exactTarget) {
        operation.future.setCancelAction(() -> cancel(operation));
        operation.future.whenComplete((ignored, error) -> {
            ScheduledFuture<?> timer = operation.deadlineTask;
            if (timer != null)
                timer.cancel(false);
            if (operation.requestId != 0)
                replies.remove(operation.requestId);
            synchronized (lock) {
                lifecycle.remove(operation);
            }
        });

        boolean rejectClosed;
        synchronized (lock) {
            rejectClosed = closed || closing;
            if (!rejectClosed)
                lifecycle.add(operation);
        }
        if (rejectClosed) {
            failClosed(operation);
            return;
        }
        scheduleDeadline(operation);
        execute(() -> selectAndQueue(operation, selector, exactTarget),
            operation);
    }

    private void selectAndQueue(Operation<?> operation, RoutingId selector,
                                Target exactTarget) {
        if (!canSelect(operation))
            return;

        Selection selection;
        try {
            selection = exactTarget == null
                ? nativeAccess.select(selector)
                : new Selection(SubmitResult.OK, exactTarget, 0);
        } catch (Throwable error) {
            fail(operation, error);
            return;
        }
        if (selection.result() != SubmitResult.OK) {
            fail(operation, new ZlinkSubmitException(selection.result(),
                selection.nativeErrno()));
            return;
        }

        boolean schedule = false;
        boolean armDeadline = false;
        synchronized (lock) {
            if (closed || closing || operation.future.isDone()
                || operation.phase == Phase.TERMINAL) {
                return;
            }
            operation.target = selection.target();
            ArrayDeque<Operation<?>> queue = pendingByTarget.computeIfAbsent(
                operation.target, ignored -> new ArrayDeque<>());
            boolean first = queue.isEmpty();
            if (!first && operation.attemptBeforeExpiry) {
                operation.attemptBeforeExpiry = false;
                armDeadline = true;
            }
            operation.phase = first ? Phase.READY : Phase.QUEUED;
            queue.addLast(operation);
            if (first) {
                markReadyLocked(operation.target);
                schedule = requestPumpLocked();
            }
        }
        if (armDeadline)
            scheduleDeadline(operation);
        if (schedule)
            executePump();
    }

    private boolean canSelect(Operation<?> operation) {
        synchronized (lock) {
            if (closed || closing || operation.future.isDone()
                || operation.phase == Phase.TERMINAL) {
                return false;
            }
            if (expired(operation, System.nanoTime())
                && !operation.attemptBeforeExpiry) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
            } else {
                return true;
            }
        }
        completeTimeout(operation);
        return false;
    }

    void onReady(Target target, int state, int terminalErrno) {
        Objects.requireNonNull(target, "target");
        boolean schedule = false;
        synchronized (lock) {
            if (closed)
                return;
            ArrayDeque<Operation<?>> queue = pendingByTarget.get(target);
            if (queue == null || queue.isEmpty())
                return;
            if (state == ROUTED_TERMINAL) {
                Throwable terminal = new ZlinkSubmitException(
                    terminalResult(terminalErrno), terminalErrno);
                for (Operation<?> operation : queue)
                    operation.forcedFailure = terminal;
                Operation<?> front = queue.peekFirst();
                if (front != null && front.phase != Phase.IN_FLIGHT) {
                    front.phase = Phase.READY;
                    markReadyLocked(target);
                    schedule = requestPumpLocked();
                }
            } else if (state == ROUTED_WRITABLE) {
                wakeVersions.merge(target, 1L, Long::sum);
                Operation<?> front = queue.peekFirst();
                if (front != null && front.phase == Phase.WAITING) {
                    front.phase = Phase.READY;
                    markReadyLocked(target);
                    schedule = requestPumpLocked();
                }
            }
        }
        if (schedule)
            executePump();
    }

    private void pump() {
        synchronized (lock) {
            pumpScheduled = false;
            if (closed || closing || pumping)
                return;
            pumping = true;
        }
        boolean scheduleAgain = false;
        try {
            for (;;) {
                AttemptWork work = nextAttempt();
                if (work == null)
                    break;
                if (work.terminalFailure != null) {
                    completeFailure(work.operation, work.terminalFailure);
                    continue;
                }
                AttemptResult result;
                try {
                    result = attempt(work.operation);
                } catch (Throwable error) {
                    result = new AttemptResult(SubmitResult.INTERNAL_ERROR,
                        0, error);
                }
                finishAttempt(work.operation, result);
            }
        } finally {
            synchronized (lock) {
                pumping = false;
                lock.notifyAll();
                if (!closed && !closing && !readySet.isEmpty())
                    scheduleAgain = requestPumpLocked();
            }
            if (scheduleAgain)
                executePump();
        }
    }

    private AttemptWork nextAttempt() {
        for (;;) {
            Operation<?> operation;
            Throwable terminal = null;
            synchronized (lock) {
                if (closed || closing)
                    return null;
                Target target = readyTargets.pollFirst();
                if (target == null)
                    return null;
                readySet.remove(target);
                ArrayDeque<Operation<?>> queue = pendingByTarget.get(target);
                if (queue == null || queue.isEmpty())
                    continue;
                operation = queue.peekFirst();
                if (operation == null || operation.phase != Phase.READY)
                    continue;
                if (operation.forcedFailure != null) {
                    terminal = operation.forcedFailure;
                    removeOperationLocked(operation);
                    operation.phase = Phase.TERMINAL;
                    lifecycle.remove(operation);
                } else if (expired(operation, System.nanoTime())
                           && !(operation.attemptBeforeExpiry
                                && !operation.attempted)) {
                    terminal = timeoutFailure(operation);
                    removeOperationLocked(operation);
                    operation.phase = Phase.TERMINAL;
                    lifecycle.remove(operation);
                } else {
                    operation.phase = Phase.IN_FLIGHT;
                    operation.attempted = true;
                    operation.observedWake = wakeVersions.getOrDefault(
                        target, 0L);
                    activeAttempts++;
                }
            }
            return new AttemptWork(operation, terminal);
        }
    }

    private AttemptResult attempt(Operation<?> operation) {
        if (operation.requestId == 0)
            return nativeAccess.send(operation.target,
                operation.snapshot.parts());
        int remaining = remainingTimeoutMillis(operation.deadlineNanos);
        return nativeAccess.request(operation.target,
            operation.snapshot.parts(),
            remaining, operation.requestId, replies);
    }

    private void finishAttempt(Operation<?> operation, AttemptResult result) {
        Throwable failure = result.failure();
        boolean nativeAccepted = result.result() == SubmitResult.OK;
        boolean completeAccepted = false;
        synchronized (lock) {
            activeAttempts--;
            lock.notifyAll();
            if (operation.phase == Phase.TERMINAL) {
                // Cancellation already completed the public future. We still
                // consume borrowed Java inputs when Core accepted the record.
            } else if (closed) {
                failure = new ZlinkSubmitException(SubmitResult.TERMINATED,
                    result.nativeErrno());
                removeOperationLocked(operation);
            } else if (operation.forcedFailure != null) {
                failure = operation.forcedFailure;
                removeOperationLocked(operation);
            } else if (result.result() == SubmitResult.OK) {
                completeAccepted = true;
                removeOperationLocked(operation);
                operation.phase = operation.requestId == 0
                    ? Phase.TERMINAL : Phase.ACCEPTED;
            } else if (result.result() == SubmitResult.BACKPRESSURED) {
                if (expired(operation, System.nanoTime())) {
                    failure = timeoutFailure(operation);
                    removeOperationLocked(operation);
                } else if (wakeVersions.getOrDefault(operation.target, 0L)
                           > operation.observedWake) {
                    operation.phase = Phase.READY;
                    markReadyLocked(operation.target);
                } else {
                    operation.phase = Phase.WAITING;
                }
            } else {
                if (failure == null) {
                    failure = new ZlinkSubmitException(result.result(),
                        result.nativeErrno());
                }
                removeOperationLocked(operation);
            }
            if (!completeAccepted && failure != null) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
            } else if (completeAccepted && operation.future.isDone()) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
            }
        }

        if (nativeAccepted)
            operation.consumeAcceptedSources();
        else if (operation.phase == Phase.TERMINAL)
            operation.snapshot.release();
        if (completeAccepted) {
            if (operation.requestId == 0)
                operation.future.complete(null);
            // Core owns reply delivery after request acceptance.
        } else if (failure != null) {
            completeFailure(operation, failure);
        }
    }

    private void cancel(Operation<?> operation) {
        boolean releaseSnapshot = false;
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL)
                return;
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.forcedFailure = new java.util.concurrent
                    .CancellationException("routed operation cancelled");
                return;
            }
            if (operation.target != null)
                removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
            releaseSnapshot = true;
        }
        if (operation.requestId != 0)
            replies.remove(operation.requestId);
        if (releaseSnapshot)
            operation.snapshot.release();
    }

    private void expire(Operation<?> operation) {
        Throwable failure;
        synchronized (lock) {
            if (operation.future.isDone() || operation.phase == Phase.TERMINAL)
                return;
            if (operation.attemptBeforeExpiry && !operation.attempted)
                return;
            failure = timeoutFailure(operation);
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.forcedFailure = failure;
                return;
            }
            if (operation.target != null)
                removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
        }
        completeFailure(operation, failure);
    }

    void prepareClose() {
        boolean interrupted = false;
        synchronized (lock) {
            if (closed)
                return;
            closing = true;
            while (activeAttempts != 0 || pumping
                   || activeNativeCallbacks != 0) {
                try {
                    lock.wait();
                } catch (InterruptedException ignored) {
                    interrupted = true;
                }
            }
        }
        if (interrupted)
            Thread.currentThread().interrupt();
    }

    void abortClose() {
        boolean schedule = false;
        synchronized (lock) {
            if (closed || !closing)
                return;
            closing = false;
            schedule = requestPumpLocked();
        }
        if (schedule)
            executePump();
    }

    void commitClose() {
        Map<Operation<?>, Throwable> completeNow = new IdentityHashMap<>();
        synchronized (lock) {
            closing = false;
            closed = true;
            for (Operation<?> operation : new ArrayList<>(lifecycle)) {
                if (operation.future.isDone())
                    continue;
                Throwable failure = operation.phase == Phase.ACCEPTED
                    ? new ZlinkRequestException(RequestResult.TERMINATED)
                    : new ZlinkSubmitException(SubmitResult.TERMINATED);
                if (operation.phase == Phase.IN_FLIGHT) {
                    operation.forcedFailure = failure;
                    continue;
                }
                if (operation.target != null)
                    removeOperationLocked(operation);
                operation.phase = Phase.TERMINAL;
                completeNow.put(operation, failure);
            }
            readyTargets.clear();
            readySet.clear();
        }
        completeNow.forEach(RoutedAdmission::completeFailure);
    }

    void beginClose() {
        prepareClose();
        commitClose();
    }

    void finishClose() {
        RuntimeResources.closeArena(callbackArena);
        callbackArena = null;
        if (ownsExecutor && executor instanceof ExecutorService service)
            RuntimeResources.shutdownExecutor(service);
    }

    private void removeOperationLocked(Operation<?> operation) {
        if (operation.target != null) {
            ArrayDeque<Operation<?>> queue = pendingByTarget.get(
                operation.target);
            if (queue != null) {
                boolean wasFront = queue.peekFirst() == operation;
                queue.remove(operation);
                if (queue.isEmpty()) {
                    pendingByTarget.remove(operation.target);
                    readySet.remove(operation.target);
                    wakeVersions.remove(operation.target);
                } else if (wasFront) {
                    Operation<?> next = queue.peekFirst();
                    if (next.phase == Phase.QUEUED)
                        next.phase = Phase.READY;
                    if (next.phase == Phase.READY)
                        markReadyLocked(operation.target);
                }
            }
        }
        if (operation.requestId == 0)
            lifecycle.remove(operation);
        operation.phase = operation.requestId == 0
            ? Phase.TERMINAL : operation.phase;
    }

    private void scheduleDeadline(Operation<?> operation) {
        if (operation.deadlineNanos == Long.MAX_VALUE
            || operation.attemptBeforeExpiry)
            return;
        long delay = Math.max(0L,
            operation.deadlineNanos - System.nanoTime());
        operation.deadlineTask = deadlines.schedule(() -> expire(operation),
            delay, TimeUnit.NANOSECONDS);
    }

    private void failClosed(Operation<?> operation) {
        operation.phase = Phase.TERMINAL;
        completeFailure(operation,
            new ZlinkSubmitException(SubmitResult.TERMINATED));
    }

    private void fail(Operation<?> operation, Throwable error) {
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL)
                return;
            if (operation.target != null)
                removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
        }
        completeFailure(operation, error);
    }

    private void completeTimeout(Operation<?> operation) {
        completeFailure(operation, timeoutFailure(operation));
    }

    private static void completeFailure(Operation<?> operation,
                                        Throwable failure) {
        operation.snapshot.release();
        operation.future.completeExceptionally(failure);
    }

    private boolean requestPumpLocked() {
        if (closed || closing || pumping || pumpScheduled
            || readySet.isEmpty())
            return false;
        pumpScheduled = true;
        return true;
    }

    private void markReadyLocked(Target target) {
        if (readySet.add(target))
            readyTargets.addLast(target);
    }

    private void executePump() {
        try {
            executor.execute(this::pump);
        } catch (RejectedExecutionException rejected) {
            beginClose();
        }
    }

    private void execute(Runnable action, Operation<?> operation) {
        try {
            executor.execute(action);
        } catch (RejectedExecutionException rejected) {
            fail(operation, rejected);
        }
    }

    private void installReadyCallback(MemorySegment socketHandle) {
        Arena arena = Arena.ofShared();
        boolean installed = false;
        try {
            MemorySegment stub = LINKER.upcallStub(readyCallbackHandle(),
                READY_CALLBACK_DESCRIPTOR, arena);
            int rc = Native.routedSendReadyHandler(socketHandle, stub,
                MemorySegment.NULL);
            if (rc != 0)
                throw ZlinkException.fromLastError(ErrorCategory.HANDLER);
            callbackArena = arena;
            installed = true;
        } finally {
            if (!installed) {
                RuntimeResources.closeArena(arena);
                if (ownsExecutor && executor instanceof ExecutorService service)
                    RuntimeResources.shutdownExecutor(service);
            }
        }
    }

    private void ensureReadyCallbackInstalled() {
        if (readyCallbackSocketHandle.address() == 0)
            return;
        synchronized (lock) {
            if (callbackArena != null)
                return;
            if (closed || closing)
                throw new ZlinkSubmitException(SubmitResult.TERMINATED);
            installReadyCallback(readyCallbackSocketHandle);
        }
    }

    private MethodHandle readyCallbackHandle() {
        try {
            return MethodHandles.lookup().findVirtual(RoutedAdmission.class,
                "handleNativeReady", MethodType.methodType(void.class,
                    MemorySegment.class, MemorySegment.class,
                    MemorySegment.class)).bindTo(this);
        } catch (ReflectiveOperationException error) {
            throw new IllegalStateException(
                "failed to bind routed readiness callback", error);
        }
    }

    private void handleNativeReady(MemorySegment subject,
                                   MemorySegment event,
                                   MemorySegment userdata) {
        enterNativeCallback();
        try {
            if (event == null || event.address() == 0)
                return;
            MemorySegment value = event.reinterpret(
                NativeLayouts.ROUTED_SEND_READY_EVENT_LAYOUT.byteSize());
            RoutingId routingId = NativeRoutingIds.read(
                value.asSlice(0, NativeLayouts.ROUTING_ID_LAYOUT.byteSize()));
            Target target = new Target(routingId,
                value.get(ValueLayout.JAVA_LONG,
                    NativeLayouts.ROUTED_SEND_READY_PAIR_ID_OFFSET),
                value.get(ValueLayout.JAVA_LONG,
                    NativeLayouts.ROUTED_SEND_READY_GENERATION_OFFSET));
            int state = value.get(ValueLayout.JAVA_INT,
                NativeLayouts.ROUTED_SEND_READY_STATE_OFFSET);
            int terminalErrno = value.get(ValueLayout.JAVA_INT,
                NativeLayouts.ROUTED_SEND_READY_ERRNO_OFFSET);
            onReady(target, state, terminalErrno);
        } catch (Throwable ignored) {
            // Native callbacks must never unwind through the foreign boundary.
        } finally {
            exitNativeCallback();
        }
    }

    private void enterNativeCallback() {
        synchronized (lock) {
            activeNativeCallbacks++;
        }
    }

    private void exitNativeCallback() {
        synchronized (lock) {
            activeNativeCallbacks--;
            lock.notifyAll();
        }
    }

    private static boolean expired(Operation<?> operation, long now) {
        return operation.deadlineNanos != Long.MAX_VALUE
            && now >= operation.deadlineNanos;
    }

    private static Throwable timeoutFailure(Operation<?> operation) {
        return operation.requestId == 0
            ? new ZlinkSubmitException(SubmitResult.BACKPRESSURED)
            : new ZlinkRequestException(RequestResult.TIMED_OUT);
    }

    private static SubmitResult terminalResult(int errno) {
        if (errno == NativeErrorCodes.ENOENT
            || errno == NativeErrorCodes.EHOSTUNREACH
            || errno == NativeErrorCodes.EHOSTUNREACH_WIN)
            return SubmitResult.NOT_FOUND;
        if (errno == NativeErrorCodes.ENOTCONN
            || errno == NativeErrorCodes.ENOTCONN_WIN
            || errno == NativeErrorCodes.ECONNREFUSED
            || errno == NativeErrorCodes.ECONNREFUSED_WIN)
            return SubmitResult.NOT_CONNECTED;
        return SubmitResult.TERMINATED;
    }

    private static long saturatingDeadline(long started, long timeoutMs) {
        if (timeoutMs >= Long.MAX_VALUE / NANOS_PER_MILLI)
            return Long.MAX_VALUE;
        long delta = timeoutMs * NANOS_PER_MILLI;
        long deadline = started + delta;
        return deadline < started ? Long.MAX_VALUE : deadline;
    }

    private static int remainingTimeoutMillis(long deadline) {
        long remaining = deadline - System.nanoTime();
        if (remaining <= 0)
            return 1;
        long millis = (remaining + NANOS_PER_MILLI - 1) / NANOS_PER_MILLI;
        return RequestReplySupport.toTimeoutInt(millis);
    }

    private static PartSnapshot snapshotSharedParts(List<Message> sources) {
        List<Message> retained = new ArrayList<>(sources.size());
        try {
            for (int i = 0; i < sources.size(); i++) {
                retained.add(InternalAccess.messageSharedCopyOf(
                    Objects.requireNonNull(sources.get(i),
                        "parts[" + i + "]")));
            }
        } catch (Throwable error) {
            closeParts(retained);
            throw error;
        }
        return new PartSnapshot(retained, () -> closeParts(retained));
    }

    private static void closeParts(List<Message> parts) {
        for (Message part : parts) {
            if (part == null)
                continue;
            try {
                part.close();
            } catch (RuntimeException ignored) {
            }
        }
    }

    @FunctionalInterface
    private interface PartSnapshotter {
        PartSnapshot snapshot(List<Message> sources);
    }

    private static final class PartSnapshot {
        private final List<Message> parts;
        private final Runnable closer;
        private final AtomicBoolean released = new AtomicBoolean();

        private PartSnapshot(List<Message> parts, Runnable closer) {
            this.parts = Objects.requireNonNull(parts, "parts");
            this.closer = Objects.requireNonNull(closer, "closer");
        }

        private static PartSnapshot borrowed(List<Message> parts) {
            return new PartSnapshot(parts, () -> { });
        }

        private List<Message> parts() {
            return parts;
        }

        private void release() {
            if (released.compareAndSet(false, true))
                closer.run();
        }
    }

    record Target(RoutingId routingId, long transportPairId,
                  long transportPairGeneration) {
        Target {
            Objects.requireNonNull(routingId, "routingId");
            if (transportPairId == 0 || transportPairGeneration == 0)
                throw new IllegalArgumentException(
                    "transport pair identity must be non-zero");
        }
    }

    record Selection(SubmitResult result, Target target, int nativeErrno) {
        Selection {
            Objects.requireNonNull(result, "result");
            if (result == SubmitResult.OK)
                Objects.requireNonNull(target, "target");
        }
    }

    record AttemptResult(SubmitResult result, int nativeErrno,
                         Throwable failure) {
        AttemptResult(SubmitResult result, int nativeErrno) {
            this(result, nativeErrno, null);
        }
    }

    interface NativeAccess {
        Selection select(RoutingId selector);

        AttemptResult send(Target target, List<Message> parts);

        AttemptResult request(Target target, List<Message> parts,
                              int timeoutMs, long requestId,
                              ReplyRegistry replies);
    }

    interface ReplyRegistry {
        long nextRequestId();

        void register(long requestId,
                      CompletableFuture<List<Message>> future,
                      RoutedRequestSupport.CallbackLifecycle callbackLifecycle);

        void remove(long requestId);

        MemorySegment callback();

        MemorySegment userData(long requestId);
    }

    private static final class PanamaNativeAccess implements NativeAccess {
        private final MemorySegment socketHandle;
        private final OutboundRecordAttemptGate attemptGate;
        private final boolean dealer;

        private PanamaNativeAccess(MemorySegment socketHandle,
                                   boolean dealer,
                                   OutboundRecordAttemptGate attemptGate) {
            this.socketHandle = Objects.requireNonNull(socketHandle,
                "socketHandle");
            this.attemptGate = Objects.requireNonNull(attemptGate,
                "attemptGate");
            this.dealer = dealer;
        }

        @Override
        public Selection select(RoutingId selector) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment nativeSelector = selector == null
                    ? MemorySegment.NULL
                    : NativeRoutingIds.allocate(arena, selector);
                MemorySegment nativeTarget = arena.allocate(
                    NativeLayouts.ROUTED_SUBMIT_TARGET_LAYOUT);
                int rc = Native.selectRoutedSubmitTarget(socketHandle,
                    nativeSelector, nativeTarget);
                int nativeErrno = rc == SubmitResult.OK.value()
                    ? 0 : Native.errno();
                SubmitResult result = SubmitResult.fromValue(rc);
                if (result != SubmitResult.OK)
                    return new Selection(result, null, nativeErrno);
                return new Selection(result, readTarget(nativeTarget), 0);
            }
        }

        @Override
        public AttemptResult send(Target target, List<Message> parts) {
            return submitParts(target, parts, (nativeTarget, nativeRid,
                                                nativePart, partFlag) -> dealer
                ? Native.dealerSendTransportPairPart(socketHandle,
                    nativeTarget, nativePart, DONT_WAIT, partFlag)
                : Native.sendPartTransportPair(socketHandle, nativeRid,
                    target.transportPairId(),
                    target.transportPairGeneration(), nativePart,
                    DONT_WAIT, partFlag));
        }

        @Override
        public AttemptResult request(Target target, List<Message> parts,
                                     int timeoutMs, long requestId,
                                     ReplyRegistry replies) {
            return submitParts(target, parts, (nativeTarget, nativeRid,
                                                nativePart, partFlag) -> dealer
                ? Native.dealerRequestTransportPairPart(socketHandle,
                    nativeTarget, nativePart, DONT_WAIT, partFlag,
                    partFlag == Native.PART_FINAL ? timeoutMs : 0,
                    partFlag == Native.PART_FINAL
                        ? replies.callback() : MemorySegment.NULL,
                    partFlag == Native.PART_FINAL
                        ? replies.userData(requestId) : MemorySegment.NULL)
                : Native.routerRequestTransportPairPart(socketHandle, nativeRid,
                    target.transportPairId(),
                    target.transportPairGeneration(), nativePart,
                    DONT_WAIT, partFlag,
                    partFlag == Native.PART_FINAL ? timeoutMs : 0,
                    partFlag == Native.PART_FINAL
                        ? replies.callback() : MemorySegment.NULL,
                    partFlag == Native.PART_FINAL
                        ? replies.userData(requestId) : MemorySegment.NULL));
        }

        private AttemptResult submitParts(Target target, List<Message> parts,
                                          PartSubmit submit) {
            try (Arena arena = Arena.ofConfined()) {
                MemorySegment nativeTarget = allocateTarget(arena, target);
                MemorySegment nativeRid = nativeTarget.asSlice(0,
                    NativeLayouts.ROUTING_ID_LAYOUT.byteSize());
                MessagePartsBuffer validated = new MessagePartsBuffer();
                for (int i = 0; i < parts.size(); i++) {
                    validated.add(Objects.requireNonNull(parts.get(i),
                        "parts[" + i + "]"));
                }
                MemorySegment nativeParts = validated.copyToNativeArray(arena);
                long stride = NativeLayouts.MESSAGE_LAYOUT.byteSize();
                try {
                    return attemptGate.call(() -> {
                        for (int i = 0; i < parts.size(); i++) {
                            MemorySegment nativePart = nativeParts.asSlice(
                                i * stride, stride);
                            int partFlag = i + 1 < parts.size()
                                ? Native.PART_MORE : Native.PART_FINAL;
                            int rc = submit.submit(nativeTarget, nativeRid,
                                nativePart, partFlag);
                            if (rc != SubmitResult.OK.value()) {
                                return new AttemptResult(
                                    SubmitResult.fromValue(rc), Native.errno());
                            }
                        }
                        return new AttemptResult(SubmitResult.OK, 0);
                    });
                } finally {
                    MessagePartsBuffer.closeNativeArray(nativeParts,
                        parts.size());
                }
            }
        }

        private static MemorySegment allocateTarget(Arena arena,
                                                     Target target) {
            MemorySegment out = arena.allocate(
                NativeLayouts.ROUTED_SUBMIT_TARGET_LAYOUT);
            NativeRoutingIds.write(out, target.routingId());
            out.set(ValueLayout.JAVA_LONG,
                NativeLayouts.ROUTED_SUBMIT_TARGET_PAIR_ID_OFFSET,
                target.transportPairId());
            out.set(ValueLayout.JAVA_LONG,
                NativeLayouts.ROUTED_SUBMIT_TARGET_GENERATION_OFFSET,
                target.transportPairGeneration());
            return out;
        }

        private static Target readTarget(MemorySegment value) {
            RoutingId routingId = NativeRoutingIds.read(
                value.asSlice(0, NativeLayouts.ROUTING_ID_LAYOUT.byteSize()));
            return new Target(routingId,
                value.get(ValueLayout.JAVA_LONG,
                    NativeLayouts.ROUTED_SUBMIT_TARGET_PAIR_ID_OFFSET),
                value.get(ValueLayout.JAVA_LONG,
                    NativeLayouts.ROUTED_SUBMIT_TARGET_GENERATION_OFFSET));
        }

        @FunctionalInterface
        private interface PartSubmit {
            int submit(MemorySegment target, MemorySegment routingId,
                       MemorySegment part, int partFlag);
        }
    }

    private static final class NativeReplyRegistry implements ReplyRegistry {
        @Override
        public long nextRequestId() {
            return RoutedRequestSupport.nextRequestId();
        }

        @Override
        public void register(long requestId,
                             CompletableFuture<List<Message>> future,
                             RoutedRequestSupport.CallbackLifecycle
                                 callbackLifecycle) {
            RoutedRequestSupport.registerRoutedPending(requestId, future,
                callbackLifecycle);
        }

        @Override
        public void remove(long requestId) {
            RoutedRequestSupport.removeRoutedPending(requestId);
        }

        @Override
        public MemorySegment callback() {
            return RoutedRequestSupport.replyCallback();
        }

        @Override
        public MemorySegment userData(long requestId) {
            return RoutedRequestSupport.userData(requestId);
        }
    }

    private static final class SharedDeadlines {
        private static final ScheduledExecutorService INSTANCE =
            java.util.concurrent.Executors.newSingleThreadScheduledExecutor(
                runnable -> {
                    Thread thread = new Thread(runnable,
                        "zlink-routed-deadline");
                    thread.setDaemon(true);
                    return thread;
                });
    }

    private static final class OperationFuture<T>
      extends CompletableFuture<T> {
        private volatile Runnable cancelAction;

        private void setCancelAction(Runnable value) {
            cancelAction = value;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean cancelled = super.cancel(mayInterruptIfRunning);
            if (cancelled) {
                Runnable action = cancelAction;
                if (action != null)
                    action.run();
            }
            return cancelled;
        }
    }

    private static final class Operation<T> {
        private final OperationFuture<T> future;
        private final List<Message> sourceParts;
        private final PartSnapshot snapshot;
        private final long deadlineNanos;
        private boolean attemptBeforeExpiry;
        private final long requestId;
        private final AtomicBoolean sourcesConsumed = new AtomicBoolean();
        private Target target;
        private Phase phase = Phase.SELECTING;
        private boolean attempted;
        private long observedWake;
        private Throwable forcedFailure;
        private volatile ScheduledFuture<?> deadlineTask;

        private Operation(OperationFuture<T> future,
                          List<Message> sourceParts,
                          PartSnapshot snapshot,
                          long deadlineNanos, boolean attemptBeforeExpiry,
                          long requestId) {
            this.future = future;
            this.sourceParts = sourceParts;
            this.snapshot = snapshot;
            this.deadlineNanos = deadlineNanos;
            this.attemptBeforeExpiry = attemptBeforeExpiry;
            this.requestId = requestId;
        }

        private static Operation<Void> send(OperationFuture<Void> future,
                                            List<Message> sourceParts,
                                            PartSnapshot snapshot,
                                            long deadlineNanos,
                                            boolean attemptBeforeExpiry) {
            return new Operation<>(future, sourceParts, snapshot, deadlineNanos,
                attemptBeforeExpiry, 0L);
        }

        private static Operation<List<Message>> request(
                OperationFuture<List<Message>> future,
                List<Message> sourceParts, PartSnapshot snapshot,
                long deadlineNanos, long requestId) {
            return new Operation<>(future, sourceParts, snapshot,
                deadlineNanos, false, requestId);
        }

        private void consumeAcceptedSources() {
            snapshot.release();
            if (sourcesConsumed.compareAndSet(false, true))
                closeParts(sourceParts);
        }
    }

    private enum Phase {
        SELECTING,
        QUEUED,
        READY,
        WAITING,
        IN_FLIGHT,
        ACCEPTED,
        TERMINAL
    }

    private record AttemptWork(Operation<?> operation,
                               Throwable terminalFailure) {}
}
