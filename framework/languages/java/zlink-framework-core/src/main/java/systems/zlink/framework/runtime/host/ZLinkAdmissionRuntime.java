package systems.zlink.framework.runtime.host;

import java.time.Duration;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
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

    static java.util.function.BiFunction<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey,
        java.util.function.BiFunction<
            java.util.function.Supplier<Boolean>,
            Runnable,
            java.util.concurrent.CompletionStage<Void>>> factory(
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

    static java.util.function.BiFunction<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey,
        java.util.function.BiFunction<
            java.util.function.Supplier<Boolean>,
            Runnable,
            java.util.concurrent.CompletionStage<Void>>> factory(
        java.util.function.Function<ZLinkBackendObject, ZLinkBackendObject> source,
        java.util.function.Function<ZLinkBackendObject, Duration> timeout,
        java.util.function.ToIntFunction<ZLinkBackendObject> capacity,
        java.util.function.BiConsumer<
            ZLinkBackendObject,
            java.util.function.Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        java.util.function.BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar) {
        return (backend, key) -> (submission, cleanup) ->
            submit(
                backend,
                key,
                submission,
                cleanup,
                source,
                timeout,
                capacity,
                readyRegistrar,
                shutdownRegistrar);
    }

    static CompletionStage<Void> submit(
        ZLinkBackendObject backend,
        ZLinkBackendAdmissionKey key,
        Supplier<Boolean> attempt,
        Runnable cleanup,
        java.util.function.Function<ZLinkBackendObject, ZLinkBackendObject>
            sourceResolver,
        java.util.function.Function<ZLinkBackendObject, Duration> timeoutResolver,
        java.util.function.ToIntFunction<ZLinkBackendObject> capacityResolver,
        java.util.function.BiConsumer<
            ZLinkBackendObject,
            java.util.function.Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        java.util.function.BiConsumer<ZLinkBackendObject, Runnable>
            shutdownRegistrar) {
        ZLinkBackendObject admissionSource = sourceResolver.apply(backend);
        return source(
            admissionSource,
            capacityResolver.applyAsInt(admissionSource),
            readyRegistrar,
            shutdownRegistrar).submit(
            key,
            attempt,
            cleanup,
            normalizedTimeoutMillis(timeoutResolver.apply(admissionSource)));
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
        java.util.function.BiConsumer<
            ZLinkBackendObject,
            java.util.function.Consumer<ZLinkBackendAdmissionKey>> readyRegistrar,
        java.util.function.BiConsumer<ZLinkBackendObject, Runnable>
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
            int timeoutMillis) {
            Pending item = new Pending(this, key, attempt, cleanup);
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
                    Pending item = queue == null ? null : pollLive(queue);
                    if (queue != null && queue.isEmpty()) {
                        queues.remove(key);
                    }
                    if (item != null) {
                        items.add(item);
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

        private void drive(Pending item, int timeoutMillis) {
            while (true) {
                AwaitResult wait = reserveOrAwait(item, timeoutMillis);
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

        boolean markDone(Pending item) {
            synchronized (lock) {
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
        private boolean queued;
        private boolean reserved;
        private boolean waitingCapacity;
        private boolean done;
        private boolean cleaned;
        private long deadlineNanos;
        private ScheduledFuture<?> deadline;

        Pending(
            Source source,
            ZLinkBackendAdmissionKey key,
            Supplier<Boolean> attempt,
            Runnable cleanup) {
            this.source = source;
            this.key = key;
            this.attempt = attempt;
            this.cleanup = cleanup;
            this.future = new AdmissionFuture(this);
        }

        AttemptResult attemptOnce() {
            RuntimeException terminal = null;
            Throwable failure = null;
            synchronized (source.lock) {
                if (done) {
                    return AttemptResult.TERMINAL;
                }
                if (System.nanoTime() >= deadlineNanos) {
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
            if (!source.markDone(this)) {
                return;
            }
            completeExceptionallyMarked(
                ZLinkOneWayAdmission.result(ZLinkOneWayAdmissionStatus.TIMED_OUT));
        }

        boolean cancel() {
            if (!source.markDone(this)) {
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
            future.completeExceptionallyTerminal(error);
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
