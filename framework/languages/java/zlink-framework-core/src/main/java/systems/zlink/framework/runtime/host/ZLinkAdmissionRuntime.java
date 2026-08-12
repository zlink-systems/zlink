package systems.zlink.framework.runtime.host;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.ToIntFunction;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.WeakHashMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;
import java.util.logging.Level;
import java.util.logging.Logger;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

/** Shared bounded admission wait used by all one-way Java framework families. */
final class ZLinkAdmissionRuntime {
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkAdmissionRuntime.class.getName());
    private static final Map<ZLinkBackendObject, Source> SOURCES = new WeakHashMap<>();

    private ZLinkAdmissionRuntime() {
    }

    static ZLinkOneWayCalls.Admission factory(
        systems.zlink.framework.runtime.internal.backend
            .ZLinkBackendAdapterProvider provider) {
        var source = provider.admissionSource();
        var timeout = provider.admissionTimeout();
        var capacity = provider.admissionPendingCapacity();
        var readyRegistrar = provider.admissionReadyRegistrar();
        var shutdownRegistrar = provider.admissionShutdownRegistrar();
        return factory(
            source, timeout, capacity, readyRegistrar, shutdownRegistrar);
    }

    static ZLinkOneWayCalls.Admission factory(
        Function<ZLinkBackendObject, ZLinkBackendObject> source,
        Function<ZLinkBackendObject, Duration> timeout,
        ToIntFunction<ZLinkBackendObject> capacity,
        BiConsumer<
            ZLinkBackendObject,
            Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar) {
        return new ZLinkOneWayCalls.Admission() {
            @Override
            public CompletionStage<Void> submit(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeoutOverride) {
                return ZLinkAdmissionRuntime.submit(
                    backend,
                    key,
                    submission,
                    cleanup,
                    source,
                    timeout,
                    capacity,
                    readyRegistrar,
                    shutdownRegistrar,
                    timeoutOverride,
                    false);
            }

            @Override
            public CompletionStage<Void> submitDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Supplier<Boolean> submission,
                Runnable cleanup,
                Duration timeoutOverride) {
                return ZLinkAdmissionRuntime.submit(
                    backend,
                    key,
                    submission,
                    cleanup,
                    source,
                    timeout,
                    capacity,
                    readyRegistrar,
                    shutdownRegistrar,
                    timeoutOverride,
                    true);
            }

            @Override
            public void releaseDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key) {
                ZLinkAdmissionRuntime.releaseDetached(backend, key, source);
            }

            @Override
            public void terminateDetached(
                ZLinkBackendObject backend,
                ZLinkBackendAdmissionKey key,
                Throwable failure) {
                ZLinkAdmissionRuntime.terminateDetached(
                    backend, key, failure, source);
            }
        };
    }

    static void releaseDetached(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Function<ZLinkBackendObject, ZLinkBackendObject> sourceResolver) {
        Source current = existingSource(sourceResolver.apply(backend));
        if (current != null) {
            current.releaseDetached(key);
        }
    }

    static void terminateDetached(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Throwable failure,
        Function<ZLinkBackendObject, ZLinkBackendObject> sourceResolver) {
        Source current = existingSource(sourceResolver.apply(backend));
        if (current != null) {
            current.terminateDetached(
                key, Objects.requireNonNull(failure, "failure"));
        }
    }

    static CompletionStage<Void> submit(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Supplier<Boolean> attempt,
        Runnable cleanup,
        Function<ZLinkBackendObject, ZLinkBackendObject>
            sourceResolver,
        Function<ZLinkBackendObject, Duration> timeoutResolver,
        ToIntFunction<ZLinkBackendObject> capacityResolver,
        BiConsumer<
            ZLinkBackendObject,
            Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar) {
        return submit(
            backend,
            key,
            attempt,
            cleanup,
            sourceResolver,
            timeoutResolver,
            capacityResolver,
            readyRegistrar,
            shutdownRegistrar,
            null,
            false);
    }

    static CompletionStage<Void> submit(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Supplier<Boolean> attempt,
        Runnable cleanup,
        Function<ZLinkBackendObject, ZLinkBackendObject>
            sourceResolver,
        Function<ZLinkBackendObject, Duration> timeoutResolver,
        ToIntFunction<ZLinkBackendObject> capacityResolver,
        BiConsumer<
            ZLinkBackendObject,
            Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar,
        Duration timeoutOverride) {
        return submit(
            backend,
            key,
            attempt,
            cleanup,
            sourceResolver,
            timeoutResolver,
            capacityResolver,
            readyRegistrar,
            shutdownRegistrar,
            timeoutOverride,
            false);
    }

    static CompletionStage<Void> submit(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Supplier<Boolean> attempt,
        Runnable cleanup,
        Function<ZLinkBackendObject, ZLinkBackendObject>
            sourceResolver,
        Function<ZLinkBackendObject, Duration> timeoutResolver,
        ToIntFunction<ZLinkBackendObject> capacityResolver,
        BiConsumer<
            ZLinkBackendObject,
            Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar,
        Duration timeoutOverride,
        boolean detached) {
        ZLinkBackendObject admissionSource = sourceResolver.apply(backend);
        Duration configuredTimeout = timeoutResolver.apply(admissionSource);
        Duration effectiveTimeout = timeoutOverride == null
            || configuredTimeout.compareTo(timeoutOverride) <= 0
                ? configuredTimeout
                : timeoutOverride;
        return source(
            admissionSource,
            capacityResolver.applyAsInt(admissionSource),
            readyRegistrar,
            shutdownRegistrar).submit(
            key,
            attempt,
            cleanup,
            normalizedTimeoutMillis(effectiveTimeout),
            detached);
    }

    static int normalizedTimeoutMillis(Duration timeout) {
        if (timeout == null || timeout.isZero() || timeout.isNegative()) {
            throw new IllegalArgumentException("send timeout must be positive");
        }
        long seconds = timeout.getSeconds();
        int nanos = timeout.getNano();
        if (seconds > Integer.MAX_VALUE / 1000L) {
            throw new IllegalArgumentException("send timeout exceeds Integer.MAX_VALUE ms");
        }
        long millis = seconds * 1000L + (nanos + 999_999L) / 1_000_000L;
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new IllegalArgumentException(
                "send timeout must normalize to 1..Integer.MAX_VALUE ms");
        }
        return (int) millis;
    }

    private static Source source(
        ZLinkBackendObject backend,
        int pendingCapacity,
        BiConsumer<
            ZLinkBackendObject,
            Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar) {
        synchronized (SOURCES) {
            Source current = SOURCES.get(backend);
            if (current != null) {
                return current;
            }
            Source created = new Source(pendingCapacity);
            SOURCES.put(backend, created);
            readyRegistrar.accept(backend, created::ready);
            shutdownRegistrar.accept(backend, created::shutdown);
            return created;
        }
    }

    private static Source existingSource(ZLinkBackendObject backend) {
        synchronized (SOURCES) {
            return SOURCES.get(backend);
        }
    }

    private static final class Source {
        private final int pendingCapacity;
        private final ScheduledExecutorService deadlines =
            Executors.newSingleThreadScheduledExecutor(runnable -> {
                Thread thread = new Thread(
                    runnable,
                    "zlink-java-submit-deadline");
                thread.setDaemon(true);
                return thread;
            });
        private final Object lock = new Object();
        private final Map<ZLinkBackendAdmissionKey, ArrayDeque<Pending>> queues =
            new HashMap<>();
        private final Map<ZLinkBackendAdmissionKey, Integer> readyCredits =
            new HashMap<>();
        private final Set<Pending> pending = new HashSet<>();
        private final ArrayDeque<Pending> capacityWaiters = new ArrayDeque<>();
        private int pendingCount;
        private boolean shutdown;

        Source(int pendingCapacity) {
            if (pendingCapacity <= 0) {
                throw new IllegalArgumentException(
                    "pending admission capacity must be positive");
            }
            this.pendingCapacity = pendingCapacity;
        }

        CompletionStage<Void> submit(
            ZLinkBackendAdmissionKey key,
            Supplier<Boolean> attempt,
            Runnable cleanup,
            int timeoutMillis,
            boolean detached) {
            Pending item = new Pending(
                this, key, attempt, cleanup, detached, timeoutMillis);
            item.deadlineNanos = System.nanoTime()
                + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
            if (item.attemptOnce() == AttemptResult.RETRY) {
                item.deadline = deadlines.schedule(
                    item::timeout,
                    timeoutMillis,
                    TimeUnit.MILLISECONDS);
                drive(item, timeoutMillis);
            }
            return item.future;
        }

        void ready(ZLinkBackendAdmissionKey key) {
            ArrayList<Pending> items = new ArrayList<>();
            synchronized (lock) {
                if (shutdown) {
                    return;
                }
                if (key.kind() == ZLinkBackendAdmissionKey.Kind.NODE) {
                    ArrayList<ZLinkBackendAdmissionKey> emptyKeys =
                        new ArrayList<>();
                    for (Map.Entry<ZLinkBackendAdmissionKey, ArrayDeque<Pending>>
                        entry : queues.entrySet()) {
                        if (!key.nodeRid().equals(entry.getKey().nodeRid())) {
                            continue;
                        }
                        Pending item = pollLive(entry.getValue());
                        if (entry.getValue().isEmpty()) {
                            emptyKeys.add(entry.getKey());
                        }
                        if (item != null) {
                            items.add(item);
                        }
                    }
                    emptyKeys.forEach(queues::remove);
                } else {
                    ArrayDeque<Pending> queue = queues.get(key);
                    //  A BOUND_SESSION ready edge is a binding-state change,
                    //  not one unit of send capacity: the remote bound-session
                    //  binding was installed and every push that parked while
                    //  it was missing can now be attempted. The install emits
                    //  exactly one edge, so releasing only the queue head
                    //  would strand the rest until their send deadline. Drain
                    //  the key in FIFO order; an item whose attempt still
                    //  cannot submit re-queues through `drive`, which leaves
                    //  the capacity semantics of every other kind unchanged.
                    boolean drain = key.kind()
                        == ZLinkBackendAdmissionKey.Kind.BOUND_SESSION;
                    Pending item;
                    while (queue != null
                        && (item = pollLive(queue)) != null) {
                        items.add(item);
                        if (!drain) {
                            break;
                        }
                    }
                    if (queue != null && queue.isEmpty()) {
                        queues.remove(key);
                    }
                }
            }
            if (items.isEmpty()) {
                // Preserve one edge that races between first EAGAIN and enqueue.
                synchronized (lock) {
                    if (!shutdown) {
                        readyCredits.put(key, 1);
                    }
                }
                return;
            }
            for (Pending item : items) {
                if (item.attemptOnce() == AttemptResult.RETRY) {
                    drive(item, 0);
                }
            }
        }

        void shutdown() {
            ArrayList<Pending> terminal;
            synchronized (lock) {
                if (shutdown) {
                    return;
                }
                shutdown = true;
                terminal = new ArrayList<>(pending);
                for (Pending item : terminal) {
                    markDoneLocked(item);
                }
                queues.clear();
                readyCredits.clear();
                capacityWaiters.clear();
            }
            for (Pending item : terminal) {
                item.completeExceptionallyMarked(
                    ZLinkOneWayAdmission.result(ZLinkOneWayAdmissionStatus.SHUTDOWN));
            }
            deadlines.shutdownNow();
        }

        void releaseDetached(ZLinkBackendAdmissionKey key) {
            ArrayList<Pending> retained = new ArrayList<>();
            synchronized (lock) {
                if (shutdown) {
                    return;
                }
                for (Pending item : pending) {
                    if (item.matchesDetached(key)
                        && item.releaseRouteLocked()) {
                        retained.add(item);
                    }
                }
            }
            retained.forEach(Pending::schedulePostRouteDeadline);
            if (!retained.isEmpty()) {
                ready(key);
            }
        }

        void terminateDetached(
            ZLinkBackendAdmissionKey key,
            Throwable failure) {
            ArrayList<Pending> terminal = new ArrayList<>();
            synchronized (lock) {
                for (Pending item : new ArrayList<>(pending)) {
                    if (item.matchesDetached(key) && markDoneLocked(item)) {
                        terminal.add(item);
                    }
                }
            }
            terminal.forEach(item ->
                item.completeDetachedTerminalMarked(failure));
        }

        private void drive(Pending item, int timeoutMillis) {
            while (true) {
                AwaitResult wait = reserveOrAwait(item, timeoutMillis);
                item.acceptDetachedIfReserved();
                if (wait == AwaitResult.QUEUED) {
                    return;
                }
                if (wait == AwaitResult.RETRY) {
                    if (item.attemptOnce() == AttemptResult.RETRY) {
                        continue;
                    }
                    return;
                }
                if (wait == AwaitResult.BACKPRESSURED) {
                    item.completeExceptionallyMarked(ZLinkOneWayAdmission.result(
                        ZLinkOneWayAdmissionStatus.BACKPRESSURED));
                } else if (wait == AwaitResult.SHUTDOWN) {
                    item.completeExceptionallyMarked(ZLinkOneWayAdmission.result(
                        ZLinkOneWayAdmissionStatus.SHUTDOWN));
                }
                return;
            }
        }

        private AwaitResult reserveOrAwait(Pending item, int timeoutMillis) {
            synchronized (lock) {
                if (item.done) {
                    return AwaitResult.TERMINAL;
                }
                if (shutdown) {
                    markDoneLocked(item);
                    return AwaitResult.SHUTDOWN;
                }
                if (!item.reserved) {
                    if (pendingCount >= pendingCapacity) {
                        if (!item.waitingCapacity) {
                            if (capacityWaiters.size() >= pendingCapacity) {
                                markDoneLocked(item);
                                return AwaitResult.BACKPRESSURED;
                            }
                            item.waitingCapacity = true;
                            pending.add(item);
                            capacityWaiters.addLast(item);
                        }
                        return AwaitResult.QUEUED;
                    }
                    item.reserved = true;
                    pending.add(item);
                    pendingCount++;
                }
                if (consumeReadyCredit(item.key)) {
                    return AwaitResult.RETRY;
                }
                enqueueLocked(item);
                return AwaitResult.QUEUED;
            }
        }

        private boolean consumeReadyCredit(ZLinkBackendAdmissionKey key) {
            if (readyCredits.remove(key) != null) {
                return true;
            }
            if (key.kind() == ZLinkBackendAdmissionKey.Kind.NODE
                || key.nodeRid() == null) {
                return false;
            }
            return readyCredits.remove(
                ZLinkBackendAdmissionKey.node(key.nodeRid())) != null;
        }

        private Pending pollLive(ArrayDeque<Pending> queue) {
            Pending item;
            while ((item = queue.pollFirst()) != null) {
                item.queued = false;
                if (!item.done) {
                    return item;
                }
            }
            return null;
        }

        private void enqueueLocked(Pending item) {
            queues.computeIfAbsent(item.key, ignored -> new ArrayDeque<>())
                .addLast(item);
            item.queued = true;
        }

        boolean cancelFromCaller(Pending item) {
            synchronized (lock) {
                if (item.callerAccepted) {
                    return false;
                }
                return markDoneLocked(item);
            }
        }

        boolean timeoutWhileArmed(Pending item) {
            synchronized (lock) {
                if (item.callerAccepted && !item.routeReleased) {
                    return false;
                }
                return markDoneLocked(item);
            }
        }

        private boolean markDoneLocked(Pending item) {
            if (item.done) {
                return false;
            }
            item.done = true;
            if (item.queued) {
                ArrayDeque<Pending> queue = queues.get(item.key);
                if (queue != null) {
                    queue.remove(item);
                    if (queue.isEmpty()) {
                        queues.remove(item.key);
                    }
                }
                item.queued = false;
            }
            if (item.waitingCapacity) {
                capacityWaiters.remove(item);
                item.waitingCapacity = false;
            }
            if (item.reserved) {
                pending.remove(item);
                item.reserved = false;
                pendingCount--;
                promoteCapacityWaiterLocked();
            } else {
                pending.remove(item);
            }
            return true;
        }

        private void promoteCapacityWaiterLocked() {
            Pending promoted;
            while ((promoted = capacityWaiters.pollFirst()) != null) {
                promoted.waitingCapacity = false;
                if (promoted.done) {
                    continue;
                }
                promoted.reserved = true;
                pendingCount++;
                Pending retry = promoted;
                deadlines.execute(() -> drive(retry, 0));
                return;
            }
        }
    }

    private enum AttemptResult {
        TERMINAL,
        RETRY
    }

    private enum AwaitResult {
        QUEUED,
        RETRY,
        BACKPRESSURED,
        SHUTDOWN,
        TERMINAL
    }

    private static final class Pending {
        private final Source source;
        private final ZLinkBackendAdmissionKey key;
        private final Supplier<Boolean> attempt;
        private final Runnable cleanup;
        private final AdmissionFuture future;
        private final boolean detached;
        private boolean queued;
        private boolean reserved;
        private boolean waitingCapacity;
        private boolean done;
        private boolean cleaned;
        private boolean callerAccepted;
        private boolean routeReleased;
        private final int timeoutMillis;
        private long deadlineNanos;
        private ScheduledFuture<?> deadline;

        Pending(
            Source source,
            ZLinkBackendAdmissionKey key,
            Supplier<Boolean> attempt,
            Runnable cleanup,
            boolean detached,
            int timeoutMillis) {
            this.source = source;
            this.key = key;
            this.attempt = attempt;
            this.cleanup = cleanup;
            this.detached = detached;
            this.timeoutMillis = timeoutMillis;
            this.future = new AdmissionFuture(this);
        }

        void acceptDetachedIfReserved() {
            boolean accepted = false;
            ScheduledFuture<?> admissionDeadline = null;
            synchronized (source.lock) {
                if (detached && reserved && !done && !callerAccepted) {
                    callerAccepted = true;
                    accepted = true;
                    if (!routeReleased) {
                        admissionDeadline = deadline;
                    }
                }
            }
            if (accepted) {
                // The public send deadline governs admission into this bounded
                // owner. Once admitted, relocation route readiness owns the
                // retained attempt until transport terminal or runtime
                // shutdown; reusing the public deadline here would drop a
                // command-44-racing push before command 38 installs its route.
                if (admissionDeadline != null) {
                    admissionDeadline.cancel(false);
                }
                future.completeTerminal();
            }
        }

        boolean matchesDetached(ZLinkBackendAdmissionKey candidate) {
            return detached && key.equals(candidate);
        }

        boolean releaseRouteLocked() {
            if (done || routeReleased) {
                return false;
            }
            routeReleased = true;
            if (!callerAccepted) {
                return false;
            }
            deadlineNanos = System.nanoTime()
                + TimeUnit.MILLISECONDS.toNanos(timeoutMillis);
            return true;
        }

        void schedulePostRouteDeadline() {
            ScheduledFuture<?> task = source.deadlines.schedule(
                this::timeout,
                timeoutMillis,
                TimeUnit.MILLISECONDS);
            synchronized (source.lock) {
                if (done) {
                    task.cancel(false);
                } else {
                    deadline = task;
                }
            }
        }

        AttemptResult attemptOnce() {
            RuntimeException terminal = null;
            Throwable failure = null;
            synchronized (source.lock) {
                if (done) {
                    return AttemptResult.TERMINAL;
                }
                if ((!callerAccepted || routeReleased)
                    && System.nanoTime() >= deadlineNanos) {
                    source.markDoneLocked(this);
                    terminal = ZLinkOneWayAdmission.result(
                        ZLinkOneWayAdmissionStatus.TIMED_OUT);
                } else if (source.shutdown) {
                    source.markDoneLocked(this);
                    terminal = ZLinkOneWayAdmission.result(ZLinkOneWayAdmissionStatus.SHUTDOWN);
                } else {
                    try {
                        if (attempt.get()) {
                            source.markDoneLocked(this);
                            terminal = ZLinkOneWayAdmission.result(
                                ZLinkOneWayAdmissionStatus.SUBMITTED);
                        } else {
                            return AttemptResult.RETRY;
                        }
                    } catch (RuntimeException error) {
                        RuntimeException mapped = ZLinkOneWayAdmission.fromFailure(error);
                        if (mapped != null
                            && mapped instanceof systems.zlink.framework.errors
                                .ZLinkFrameworkException frameworkError
                            && frameworkError.kind()
                                == systems.zlink.framework.errors
                                    .ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED) {
                            return AttemptResult.RETRY;
                        }
                        source.markDoneLocked(this);
                        if (mapped != null) {
                            terminal = mapped;
                        } else {
                            failure = error;
                        }
                    }
                }
            }
            if (failure != null) {
                completeExceptionallyMarked(failure);
            } else {
                if (terminal == null) {
                    completeMarked();
                } else {
                    completeExceptionallyMarked(terminal);
                }
            }
            return AttemptResult.TERMINAL;
        }

        void timeout() {
            if (!source.timeoutWhileArmed(this)) {
                return;
            }
            completeExceptionallyMarked(
                ZLinkOneWayAdmission.result(ZLinkOneWayAdmissionStatus.TIMED_OUT));
        }

        boolean cancel() {
            if (!source.cancelFromCaller(this)) {
                return false;
            }
            cancelDeadline();
            try {
                return future.cancelTerminal();
            } finally {
                cleanupOnce();
            }
        }

        void completeMarked() {
            cancelDeadline();
            cleanupOnce();
            future.completeTerminal();
        }

        void completeExceptionallyMarked(Throwable error) {
            cancelDeadline();
            cleanupOnce();
            if (!future.completeExceptionallyTerminal(error)
                && detached && callerAccepted) {
                LOGGER.log(
                    Level.WARNING,
                    "accepted one-way transport attempt ended after local "
                        + "outbound admission for " + key,
                    error);
            }
        }

        void completeDetachedTerminalMarked(Throwable error) {
            cancelDeadline();
            cleanupOnce();
            if (!callerAccepted) {
                future.completeExceptionallyTerminal(error);
            }
        }

        private void cancelDeadline() {
            ScheduledFuture<?> task = deadline;
            if (task != null) {
                task.cancel(false);
            }
        }

        private void cleanupOnce() {
            synchronized (source.lock) {
                if (cleaned) {
                    return;
                }
                cleaned = true;
            }
            try {
                cleanup.run();
            } catch (RuntimeException error) {
                LOGGER.log(Level.WARNING, "one-way submission payload cleanup failed", error);
            }
        }
    }

    private static final class AdmissionFuture
        extends CompletableFuture<Void> {
        private final Pending pending;

        AdmissionFuture(Pending pending) {
            this.pending = pending;
        }

        @Override
        public boolean cancel(boolean mayInterruptIfRunning) {
            return pending.cancel();
        }

        boolean cancelTerminal() {
            return super.cancel(false);
        }

        boolean completeTerminal() {
            return super.complete(null);
        }

        boolean completeExceptionallyTerminal(Throwable error) {
            return super.completeExceptionally(error);
        }
    }
}
