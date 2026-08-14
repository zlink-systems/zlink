/* SPDX-License-Identifier: MPL-2.0 */

package systems.zlink.runtime.sockets;

import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.RejectedExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendReadyHandler;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.RuntimeResources;

/**
 * Socket-owned asynchronous admission for PUB and XPUB records.
 *
 * <p>The Core send-ready edge is the only retry trigger for a backpressured
 * record. This aggregate owns no byte or record capacity and performs no
 * readiness polling.
 */
final class PublisherAdmission {
    private static final int MAX_ATTEMPTS_PER_TURN = 64;
    private static final long NANOS_PER_MILLI = 1_000_000L;

    private final Object lock = new Object();
    private final NativeAccess nativeAccess;
    private final Executor executor;
    private final ScheduledExecutorService deadlines;
    private final PartSnapshotter partSnapshotter;
    private final boolean ownsExecutor;
    private final ArrayDeque<Operation> fresh = new ArrayDeque<>();
    private final ArrayDeque<Operation> parked = new ArrayDeque<>();
    private final ArrayDeque<Operation> readyWork = new ArrayDeque<>();
    private final Set<Operation> lifecycle =
        java.util.Collections.newSetFromMap(new IdentityHashMap<>());

    private SendReadyHandler observer;
    private boolean closing;
    private boolean closed;
    private boolean pumpScheduled;
    private boolean pumping;
    private Thread pumpThread;
    private int activeAttempts;
    private long readyEpoch;
    private boolean preferReady;

    PublisherAdmission(NativeSocketRuntime runtime,
                       OutboundRecordAttemptGate attemptGate) {
        this(new RuntimeNativeAccess(runtime, attemptGate),
            RuntimeResources.daemonSingleThreadExecutor(
                "zlink-publisher-admission"),
            SharedDeadlines.INSTANCE,
            PublisherAdmission::snapshotSharedParts,
            true);
        runtime.setSendReadyHandler(this::signalReady);
    }

    PublisherAdmission(NativeAccess nativeAccess, Executor executor,
                       ScheduledExecutorService deadlines) {
        this(nativeAccess, executor, deadlines, PartSnapshot::borrowed, false);
    }

    private PublisherAdmission(NativeAccess nativeAccess, Executor executor,
                               ScheduledExecutorService deadlines,
                               PartSnapshotter partSnapshotter,
                               boolean ownsExecutor) {
        this.nativeAccess = Objects.requireNonNull(nativeAccess,
            "nativeAccess");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.deadlines = Objects.requireNonNull(deadlines, "deadlines");
        this.partSnapshotter = Objects.requireNonNull(partSnapshotter,
            "partSnapshotter");
        this.ownsExecutor = ownsExecutor;
    }

    CompletionStage<Void> publish(String topic, List<Message> parts,
                                  int sendTimeoutMs) {
        Objects.requireNonNull(topic, "topic");
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty())
            throw new IllegalArgumentException("parts must not be empty");

        OperationFuture future = new OperationFuture();
        List<Message> sources = new ArrayList<>(parts);
        PartSnapshot snapshot;
        try {
            snapshot = partSnapshotter.snapshot(sources);
        } catch (Throwable error) {
            future.completeExceptionally(error);
            return future;
        }

        long started = System.nanoTime();
        long deadline = sendTimeoutMs < 0
            ? Long.MAX_VALUE
            : saturatingDeadline(started, sendTimeoutMs);
        Operation operation = new Operation(topic, sources, snapshot, future,
            deadline, sendTimeoutMs == 0);
        future.setCancelAction(() -> cancel(operation));

        boolean schedule = false;
        boolean rejectClosed;
        synchronized (lock) {
            rejectClosed = closed || closing;
            if (!rejectClosed) {
                lifecycle.add(operation);
                fresh.addLast(operation);
                schedule = requestPumpLocked();
            }
        }
        if (rejectClosed) {
            terminalFailure(operation,
                new ZlinkSubmitException(SubmitResult.TERMINATED));
            return future;
        }

        scheduleDeadline(operation);
        if (schedule)
            executePump();
        return future;
    }

    void setObserver(SendReadyHandler value) {
        synchronized (lock) {
            observer = Objects.requireNonNull(value, "handler");
        }
    }

    void signalReady() {
        SendReadyHandler currentObserver;
        boolean schedule = false;
        synchronized (lock) {
            currentObserver = observer;
            if (!closed) {
                readyEpoch++;
                preferReady = true;
                Operation operation;
                while ((operation = parked.pollFirst()) != null) {
                    if (operation.phase != Phase.PARKED)
                        continue;
                    operation.phase = Phase.READY;
                    readyWork.addLast(operation);
                }
                schedule = requestPumpLocked();
            }
        }
        if (schedule)
            executePump();
        if (currentObserver != null)
            currentObserver.onReady();
    }

    private void pump() {
        synchronized (lock) {
            pumpScheduled = false;
            if (closed || closing || pumping)
                return;
            pumping = true;
            pumpThread = Thread.currentThread();
        }

        boolean scheduleAgain = false;
        try {
            for (int count = 0; count < MAX_ATTEMPTS_PER_TURN; count++) {
                AttemptWork work = nextAttempt();
                if (work == null)
                    break;
                if (work.failure() != null) {
                    terminalFailure(work.operation(), work.failure());
                    continue;
                }

                SendResult result;
                Throwable failure = null;
                try {
                    result = nativeAccess.publish(
                        work.operation().topic,
                        work.operation().snapshot.parts());
                } catch (Throwable error) {
                    result = null;
                    failure = error;
                }
                finishAttempt(work.operation(), result, failure);
            }
        } finally {
            synchronized (lock) {
                pumping = false;
                pumpThread = null;
                lock.notifyAll();
                scheduleAgain = requestPumpLocked();
            }
            if (scheduleAgain)
                executePump();
        }
    }

    private AttemptWork nextAttempt() {
        for (;;) {
            Operation operation;
            synchronized (lock) {
                if (closed || closing)
                    return null;
                if (!readyWork.isEmpty()
                    && (fresh.isEmpty() || preferReady)) {
                    operation = readyWork.pollFirst();
                    preferReady = false;
                } else {
                    operation = fresh.pollFirst();
                    preferReady = true;
                }
                if (operation == null)
                    return null;
                if (operation.phase == Phase.TERMINAL)
                    continue;
                if (expired(operation, System.nanoTime())
                    && !(operation.attemptBeforeExpiry
                         && !operation.attempted)) {
                    removeOperationLocked(operation);
                    operation.phase = Phase.TERMINAL;
                    lifecycle.remove(operation);
                    return new AttemptWork(operation, timeoutFailure());
                }
                operation.phase = Phase.IN_FLIGHT;
                operation.attempted = true;
                operation.observedReadyEpoch = readyEpoch;
                activeAttempts++;
            }
            return new AttemptWork(operation, null);
        }
    }

    private void finishAttempt(Operation operation, SendResult result,
                               Throwable failure) {
        boolean accepted = result == SendResult.SENT;
        boolean completeAccepted = false;
        boolean releaseSnapshot = false;
        synchronized (lock) {
            activeAttempts--;
            lock.notifyAll();
            if (operation.phase == Phase.TERMINAL) {
                releaseSnapshot = !accepted;
            } else if (operation.forcedFailure != null) {
                failure = operation.forcedFailure;
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
                releaseSnapshot = !accepted;
            } else if (failure != null) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
                releaseSnapshot = true;
            } else if (accepted) {
                completeAccepted = true;
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
            } else if (result == SendResult.BACKPRESSURED) {
                if (operation.attemptBeforeExpiry
                    || expired(operation, System.nanoTime())) {
                    failure = timeoutFailure();
                    operation.phase = Phase.TERMINAL;
                    lifecycle.remove(operation);
                    releaseSnapshot = true;
                } else if (readyEpoch > operation.observedReadyEpoch) {
                    operation.phase = Phase.READY;
                    readyWork.addLast(operation);
                } else {
                    operation.phase = Phase.PARKED;
                    parked.addLast(operation);
                }
            } else {
                failure = new ZlinkSubmitException(
                    result == SendResult.NOT_READY
                        ? SubmitResult.NOT_CONNECTED
                        : SubmitResult.INTERNAL_ERROR);
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
                releaseSnapshot = true;
            }
        }

        if (accepted)
            operation.consumeAcceptedSources();
        else if (releaseSnapshot)
            operation.snapshot.release();
        if (completeAccepted)
            operation.future.complete(null);
        else if (failure != null)
            operation.future.completeExceptionally(failure);
    }

    private void cancel(Operation operation) {
        boolean release = false;
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL)
                return;
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
                return;
            }
            removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
            release = true;
        }
        if (release)
            operation.snapshot.release();
    }

    private void expire(Operation operation) {
        boolean release = false;
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL
                || operation.future.isDone()) {
                return;
            }
            if (operation.attemptBeforeExpiry && !operation.attempted)
                return;
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.forcedFailure = timeoutFailure();
                return;
            }
            removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
            release = true;
        }
        if (release) {
            operation.snapshot.release();
            operation.future.completeExceptionally(timeoutFailure());
        }
    }

    void prepareClose() {
        boolean interrupted = false;
        synchronized (lock) {
            if (closed)
                return;
            closing = true;
            while (activeAttempts != 0
                   || (pumping && pumpThread != Thread.currentThread())) {
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
        Map<Operation, Throwable> terminal = new IdentityHashMap<>();
        synchronized (lock) {
            closing = false;
            closed = true;
            for (Operation operation : new ArrayList<>(lifecycle)) {
                if (operation.phase == Phase.TERMINAL)
                    continue;
                removeOperationLocked(operation);
                operation.phase = Phase.TERMINAL;
                terminal.put(operation,
                    new ZlinkSubmitException(SubmitResult.TERMINATED));
            }
            lifecycle.clear();
            fresh.clear();
            parked.clear();
            readyWork.clear();
        }
        terminal.forEach(PublisherAdmission::terminalFailure);
    }

    void beginClose() {
        prepareClose();
        commitClose();
    }

    void finishClose() {
        if (ownsExecutor && executor instanceof ExecutorService service)
            RuntimeResources.shutdownExecutor(service);
    }

    private void scheduleDeadline(Operation operation) {
        if (operation.deadlineNanos == Long.MAX_VALUE
            || operation.attemptBeforeExpiry) {
            return;
        }
        long delay = Math.max(0L,
            operation.deadlineNanos - System.nanoTime());
        try {
            operation.deadlineTask = deadlines.schedule(
                () -> expire(operation), delay, TimeUnit.NANOSECONDS);
        } catch (RejectedExecutionException error) {
            fail(operation, error);
        }
    }

    private void fail(Operation operation, Throwable failure) {
        boolean terminal = false;
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL)
                return;
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.forcedFailure = failure;
                return;
            }
            removeOperationLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
            terminal = true;
        }
        if (terminal)
            terminalFailure(operation, failure);
    }

    private void removeOperationLocked(Operation operation) {
        fresh.remove(operation);
        parked.remove(operation);
        readyWork.remove(operation);
    }

    private boolean requestPumpLocked() {
        if (closed || closing || pumping || pumpScheduled
            || (fresh.isEmpty() && readyWork.isEmpty())) {
            return false;
        }
        pumpScheduled = true;
        return true;
    }

    private void executePump() {
        try {
            executor.execute(this::pump);
        } catch (RejectedExecutionException error) {
            List<Operation> failures = new ArrayList<>();
            synchronized (lock) {
                pumpScheduled = false;
                for (Operation operation : lifecycle) {
                    if (operation.phase == Phase.IN_FLIGHT
                        || operation.phase == Phase.TERMINAL) {
                        continue;
                    }
                    removeOperationLocked(operation);
                    operation.phase = Phase.TERMINAL;
                    failures.add(operation);
                }
                lifecycle.removeAll(failures);
            }
            failures.forEach(operation -> terminalFailure(operation, error));
        }
    }

    private static void terminalFailure(Operation operation,
                                        Throwable failure) {
        operation.snapshot.release();
        operation.future.completeExceptionally(failure);
    }

    private static ZlinkSubmitException timeoutFailure() {
        return new ZlinkSubmitException(SubmitResult.BACKPRESSURED);
    }

    private static boolean expired(Operation operation, long now) {
        return operation.deadlineNanos != Long.MAX_VALUE
            && now >= operation.deadlineNanos;
    }

    private static long saturatingDeadline(long started, int timeoutMs) {
        long duration = (long) timeoutMs * NANOS_PER_MILLI;
        long deadline = started + duration;
        return deadline < started ? Long.MAX_VALUE : deadline;
    }

    private static PartSnapshot snapshotSharedParts(List<Message> sources) {
        List<Message> retained = new ArrayList<>(sources.size());
        try {
            for (int index = 0; index < sources.size(); index++) {
                retained.add(InternalAccess.messageSharedCopyOf(
                    Objects.requireNonNull(sources.get(index),
                        "parts[" + index + "]")));
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
    interface NativeAccess {
        SendResult publish(String topic, List<Message> parts);
    }

    @FunctionalInterface
    private interface PartSnapshotter {
        PartSnapshot snapshot(List<Message> parts);
    }

    private static final class RuntimeNativeAccess implements NativeAccess {
        private final NativeSocketRuntime runtime;
        private final OutboundRecordAttemptGate attemptGate;

        private RuntimeNativeAccess(NativeSocketRuntime runtime,
                                    OutboundRecordAttemptGate attemptGate) {
            this.runtime = Objects.requireNonNull(runtime, "runtime");
            this.attemptGate = Objects.requireNonNull(attemptGate,
                "attemptGate");
        }

        @Override
        public SendResult publish(String topic, List<Message> parts) {
            return attemptGate.call(
                () -> runtime.publishNoWaitResult(topic, parts));
        }
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

    private static final class Operation {
        private final String topic;
        private final List<Message> sourceParts;
        private final PartSnapshot snapshot;
        private final OperationFuture future;
        private final long deadlineNanos;
        private final boolean attemptBeforeExpiry;
        private final AtomicBoolean sourcesConsumed = new AtomicBoolean();
        private ScheduledFuture<?> deadlineTask;
        private Phase phase = Phase.FRESH;
        private Throwable forcedFailure;
        private boolean attempted;
        private long observedReadyEpoch;

        private Operation(String topic, List<Message> sourceParts,
                          PartSnapshot snapshot, OperationFuture future,
                          long deadlineNanos, boolean attemptBeforeExpiry) {
            this.topic = topic;
            this.sourceParts = sourceParts;
            this.snapshot = snapshot;
            this.future = future;
            this.deadlineNanos = deadlineNanos;
            this.attemptBeforeExpiry = attemptBeforeExpiry;
            future.whenComplete((ignored, error) -> {
                ScheduledFuture<?> timer = deadlineTask;
                if (timer != null)
                    timer.cancel(false);
            });
        }

        private void consumeAcceptedSources() {
            snapshot.release();
            if (sourcesConsumed.compareAndSet(false, true))
                closeParts(sourceParts);
        }
    }

    private static final class OperationFuture extends CompletableFuture<Void> {
        private volatile Runnable cancelAction = () -> { };

        private void setCancelAction(Runnable value) {
            cancelAction = Objects.requireNonNull(value, "cancelAction");
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            boolean cancelled = super.cancel(mayInterruptIfRunning);
            if (cancelled)
                cancelAction.run();
            return cancelled;
        }
    }

    private record AttemptWork(Operation operation, Throwable failure) {
    }

    private enum Phase {
        FRESH,
        READY,
        IN_FLIGHT,
        PARKED,
        TERMINAL
    }

    private static final class SharedDeadlines {
        private static final ScheduledExecutorService INSTANCE =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(runnable,
                    "zlink-publisher-deadlines");
                thread.setDaemon(true);
                return thread;
            });
    }
}
