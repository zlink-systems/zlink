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
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendReadyHandler;
import systems.zlink.contracts.sockets.SendResult;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.runtime.nativeapi.InternalAccess;
import systems.zlink.runtime.nativeapi.RuntimeResources;

/** Socket-owned asynchronous admission for one complete raw STREAM record. */
final class StreamAdmission {
    private static final int MAX_ATTEMPTS_PER_TURN = 64;
    private static final long NANOS_PER_MILLI = 1_000_000L;

    private final Object lock = new Object();
    private final NativeAccess nativeAccess;
    private final Executor executor;
    private final ScheduledExecutorService deadlines;
    private final PartSnapshotter snapshotter;
    private final boolean ownsExecutor;
    private final ArrayDeque<Operation> fresh = new ArrayDeque<>();
    private final ArrayDeque<Operation> parked = new ArrayDeque<>();
    private final Set<Operation> lifecycle =
        java.util.Collections.newSetFromMap(new IdentityHashMap<>());

    private SendReadyHandler observer;
    private boolean closing;
    private boolean closed;
    private boolean pumpScheduled;
    private boolean pumping;
    private int activeAttempts;
    private long readyEpoch;

    StreamAdmission(
        NativeSocketRuntime runtime,
        OutboundRecordAttemptGate attemptGate) {
        this(
            new RuntimeNativeAccess(runtime, attemptGate),
            RuntimeResources.daemonSingleThreadExecutor(
                "zlink-stream-admission"),
            SharedDeadlines.INSTANCE,
            StreamAdmission::snapshotSharedParts,
            true);
        runtime.setSendReadyHandler(this::signalReady);
    }

    StreamAdmission(
        NativeAccess nativeAccess,
        Executor executor,
        ScheduledExecutorService deadlines) {
        this(nativeAccess, executor, deadlines, PartSnapshot::borrowed, false);
    }

    private StreamAdmission(
        NativeAccess nativeAccess,
        Executor executor,
        ScheduledExecutorService deadlines,
        PartSnapshotter snapshotter,
        boolean ownsExecutor) {
        this.nativeAccess = Objects.requireNonNull(nativeAccess, "nativeAccess");
        this.executor = Objects.requireNonNull(executor, "executor");
        this.deadlines = Objects.requireNonNull(deadlines, "deadlines");
        this.snapshotter = Objects.requireNonNull(snapshotter, "snapshotter");
        this.ownsExecutor = ownsExecutor;
    }

    CompletionStage<Void> send(
        RoutingId routingId,
        List<Message> parts,
        int sendTimeoutMs) {
        Objects.requireNonNull(routingId, "routingId");
        Objects.requireNonNull(parts, "parts");
        if (parts.isEmpty()) {
            throw new IllegalArgumentException("parts must not be empty");
        }
        List<Message> sources = new ArrayList<>(parts);
        PartSnapshot snapshot;
        try {
            snapshot = snapshotter.snapshot(sources);
        } catch (Throwable failure) {
            return CompletableFuture.failedFuture(failure);
        }
        long deadline = sendTimeoutMs < 0
            ? Long.MAX_VALUE
            : saturatingDeadline(System.nanoTime(), sendTimeoutMs);
        OperationFuture future = new OperationFuture();
        Operation operation = new Operation(
            routingId,
            snapshot,
            future,
            deadline,
            sendTimeoutMs == 0);
        future.setCancelAction(() -> cancel(operation));

        boolean schedule = false;
        boolean reject;
        synchronized (lock) {
            reject = closing || closed;
            if (!reject) {
                lifecycle.add(operation);
                fresh.addLast(operation);
                schedule = requestPumpLocked();
            }
        }
        if (reject) {
            terminalFailure(
                operation,
                new ZlinkSubmitException(SubmitResult.TERMINATED));
            return future;
        }
        scheduleDeadline(operation);
        if (schedule) {
            executePump();
        }
        return future;
    }

    void setObserver(SendReadyHandler handler) {
        synchronized (lock) {
            observer = Objects.requireNonNull(handler, "handler");
        }
    }

    void signalReady() {
        SendReadyHandler currentObserver;
        boolean schedule = false;
        synchronized (lock) {
            currentObserver = observer;
            if (!closed) {
                readyEpoch++;
                Operation operation;
                while ((operation = parked.pollFirst()) != null) {
                    if (operation.phase != Phase.PARKED) {
                        continue;
                    }
                    operation.phase = Phase.FRESH;
                    fresh.addLast(operation);
                }
                schedule = requestPumpLocked();
            }
        }
        if (schedule) {
            executePump();
        }
        if (currentObserver != null) {
            currentObserver.onReady();
        }
    }

    void prepareClose() {
        synchronized (lock) {
            if (closed || closing) {
                return;
            }
            closing = true;
            boolean interrupted = false;
            while (activeAttempts != 0) {
                try {
                    lock.wait();
                } catch (InterruptedException failure) {
                    interrupted = true;
                }
            }
            if (interrupted) {
                Thread.currentThread().interrupt();
            }
        }
    }

    void abortClose() {
        boolean schedule = false;
        synchronized (lock) {
            if (closed || !closing) {
                return;
            }
            closing = false;
            schedule = requestPumpLocked();
        }
        if (schedule) {
            executePump();
        }
    }

    void commitClose() {
        Map<Operation, Throwable> terminal = new IdentityHashMap<>();
        synchronized (lock) {
            closing = false;
            closed = true;
            for (Operation operation : new ArrayList<>(lifecycle)) {
                if (operation.phase == Phase.TERMINAL) {
                    continue;
                }
                removeLocked(operation);
                operation.phase = Phase.TERMINAL;
                terminal.put(operation,
                    new ZlinkSubmitException(SubmitResult.TERMINATED));
            }
            lifecycle.clear();
            fresh.clear();
            parked.clear();
        }
        terminal.forEach(StreamAdmission::terminalFailure);
    }

    void beginClose() {
        prepareClose();
        commitClose();
    }

    void finishClose() {
        if (ownsExecutor && executor instanceof ExecutorService service) {
            RuntimeResources.shutdownExecutor(service);
        }
    }

    private void pump() {
        synchronized (lock) {
            pumpScheduled = false;
            if (closed || closing || pumping) {
                return;
            }
            pumping = true;
        }
        boolean scheduleAgain;
        try {
            for (int count = 0; count < MAX_ATTEMPTS_PER_TURN; count++) {
                AttemptWork work = nextAttempt();
                if (work == null) {
                    break;
                }
                if (work.failure != null) {
                    terminalFailure(work.operation, work.failure);
                    continue;
                }
                SendResult result = null;
                Throwable failure = null;
                try {
                    result = nativeAccess.send(
                        work.operation.routingId,
                        work.operation.snapshot.parts());
                } catch (Throwable error) {
                    failure = error;
                }
                finishAttempt(work.operation, result, failure);
            }
        } finally {
            synchronized (lock) {
                pumping = false;
                scheduleAgain = requestPumpLocked();
            }
            if (scheduleAgain) {
                executePump();
            }
        }
    }

    private AttemptWork nextAttempt() {
        synchronized (lock) {
            if (closed || closing) {
                return null;
            }
            Operation operation = fresh.pollFirst();
            if (operation == null) {
                return null;
            }
            if (operation.phase == Phase.TERMINAL) {
                return new AttemptWork(operation, null);
            }
            if (expired(operation, System.nanoTime())
                && !(operation.attemptBeforeExpiry && !operation.attempted)) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
                return new AttemptWork(operation, timeoutFailure());
            }
            operation.phase = Phase.IN_FLIGHT;
            operation.attempted = true;
            operation.observedReadyEpoch = readyEpoch;
            activeAttempts++;
            return new AttemptWork(operation, null);
        }
    }

    private void finishAttempt(
        Operation operation,
        SendResult result,
        Throwable failure) {
        boolean success = false;
        boolean terminal = false;
        synchronized (lock) {
            activeAttempts--;
            lock.notifyAll();
            if (operation.phase == Phase.TERMINAL) {
                return;
            }
            if (operation.forcedFailure != null) {
                failure = operation.forcedFailure;
            }
            if (failure != null) {
                terminal = true;
            } else if (result == SendResult.SENT) {
                success = true;
                terminal = true;
            } else if (result == SendResult.BACKPRESSURED) {
                if (operation.attemptBeforeExpiry
                    || expired(operation, System.nanoTime())) {
                    failure = timeoutFailure();
                    terminal = true;
                } else if (readyEpoch > operation.observedReadyEpoch) {
                    operation.phase = Phase.FRESH;
                    fresh.addLast(operation);
                } else {
                    operation.phase = Phase.PARKED;
                    parked.addLast(operation);
                }
            } else {
                failure = new ZlinkSubmitException(
                    result == SendResult.NOT_READY
                        ? SubmitResult.NOT_CONNECTED
                        : SubmitResult.INTERNAL_ERROR);
                terminal = true;
            }
            if (terminal) {
                operation.phase = Phase.TERMINAL;
                lifecycle.remove(operation);
            }
        }
        if (!terminal) {
            return;
        }
        operation.snapshot.release();
        if (success) {
            operation.future.complete(null);
        } else {
            operation.future.completeExceptionally(failure);
        }
    }

    private void scheduleDeadline(Operation operation) {
        if (operation.deadlineNanos == Long.MAX_VALUE
            || operation.attemptBeforeExpiry) {
            return;
        }
        long delay = Math.max(
            0L, operation.deadlineNanos - System.nanoTime());
        try {
            operation.deadlineTask = deadlines.schedule(
                () -> fail(operation, timeoutFailure()),
                delay,
                TimeUnit.NANOSECONDS);
        } catch (RejectedExecutionException failure) {
            fail(operation, failure);
        }
    }

    private void cancel(Operation operation) {
        fail(operation, null);
    }

    private void fail(Operation operation, Throwable failure) {
        boolean terminal = false;
        synchronized (lock) {
            if (operation.phase == Phase.TERMINAL) {
                return;
            }
            if (operation.phase == Phase.IN_FLIGHT) {
                operation.forcedFailure = failure == null
                    ? new java.util.concurrent.CancellationException()
                    : failure;
                return;
            }
            removeLocked(operation);
            operation.phase = Phase.TERMINAL;
            lifecycle.remove(operation);
            terminal = true;
        }
        if (terminal) {
            operation.snapshot.release();
            if (failure != null) {
                operation.future.completeExceptionally(failure);
            }
        }
    }

    private void removeLocked(Operation operation) {
        fresh.remove(operation);
        parked.remove(operation);
    }

    private boolean requestPumpLocked() {
        if (closed || closing || pumping || pumpScheduled || fresh.isEmpty()) {
            return false;
        }
        pumpScheduled = true;
        return true;
    }

    private void executePump() {
        try {
            executor.execute(this::pump);
        } catch (RejectedExecutionException failure) {
            List<Operation> terminal = new ArrayList<>();
            synchronized (lock) {
                pumpScheduled = false;
                for (Operation operation : lifecycle) {
                    if (operation.phase == Phase.TERMINAL
                        || operation.phase == Phase.IN_FLIGHT) {
                        continue;
                    }
                    removeLocked(operation);
                    operation.phase = Phase.TERMINAL;
                    terminal.add(operation);
                }
                lifecycle.removeAll(terminal);
            }
            terminal.forEach(operation -> terminalFailure(operation, failure));
        }
    }

    private static void terminalFailure(
        Operation operation,
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
                    Objects.requireNonNull(
                        sources.get(index), "parts[" + index + "]")));
            }
        } catch (Throwable failure) {
            closeParts(retained);
            throw failure;
        }
        return new PartSnapshot(retained, () -> closeParts(retained));
    }

    private static void closeParts(List<Message> parts) {
        for (Message part : parts) {
            if (part != null) {
                try {
                    part.close();
                } catch (RuntimeException ignored) {
                }
            }
        }
    }

    @FunctionalInterface
    interface NativeAccess {
        SendResult send(RoutingId routingId, List<Message> parts);
    }

    @FunctionalInterface
    private interface PartSnapshotter {
        PartSnapshot snapshot(List<Message> parts);
    }

    private static final class RuntimeNativeAccess implements NativeAccess {
        private final NativeSocketRuntime runtime;
        private final OutboundRecordAttemptGate attemptGate;

        private RuntimeNativeAccess(
            NativeSocketRuntime runtime,
            OutboundRecordAttemptGate attemptGate) {
            this.runtime = Objects.requireNonNull(runtime, "runtime");
            this.attemptGate = Objects.requireNonNull(attemptGate, "attemptGate");
        }

        @Override
        public SendResult send(
            RoutingId routingId,
            List<Message> parts) {
            return attemptGate.call(
                () -> runtime.sendNoWaitResult(routingId, parts));
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
            if (released.compareAndSet(false, true)) {
                closer.run();
            }
        }
    }

    private static final class Operation {
        private final RoutingId routingId;
        private final PartSnapshot snapshot;
        private final OperationFuture future;
        private final long deadlineNanos;
        private final boolean attemptBeforeExpiry;
        private ScheduledFuture<?> deadlineTask;
        private Phase phase = Phase.FRESH;
        private Throwable forcedFailure;
        private boolean attempted;
        private long observedReadyEpoch;

        private Operation(
            RoutingId routingId,
            PartSnapshot snapshot,
            OperationFuture future,
            long deadlineNanos,
            boolean attemptBeforeExpiry) {
            this.routingId = routingId;
            this.snapshot = snapshot;
            this.future = future;
            this.deadlineNanos = deadlineNanos;
            this.attemptBeforeExpiry = attemptBeforeExpiry;
            future.whenComplete((ignored, failure) -> {
                ScheduledFuture<?> timer = deadlineTask;
                if (timer != null) {
                    timer.cancel(false);
                }
            });
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
            if (cancelled) {
                cancelAction.run();
            }
            return cancelled;
        }
    }

    private record AttemptWork(Operation operation, Throwable failure) {
    }

    private enum Phase {
        FRESH,
        IN_FLIGHT,
        PARKED,
        TERMINAL
    }

    private static final class SharedDeadlines {
        private static final ScheduledExecutorService INSTANCE =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(
                    runnable, "zlink-stream-deadlines");
                thread.setDaemon(true);
                return thread;
            });
    }
}
