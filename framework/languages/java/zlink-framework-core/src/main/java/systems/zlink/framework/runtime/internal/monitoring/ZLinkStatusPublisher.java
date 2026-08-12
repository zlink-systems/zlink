package systems.zlink.framework.runtime.internal.monitoring;

import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.Flow;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkObservationLoss;

/**
 * Delivers changed snapshots through bounded, per-subscriber queues.
 *
 * <p>The publisher has one dispatcher for all subscribers. A runtime calls
 * {@link #signal()} after a state change; no subscriber owns a polling thread.
 * Each subscriber retains one latest intermediate snapshot per source. A
 * separate bounded FIFO keeps preserved milestones and terminal snapshots, so
 * a busy source cannot replace another source's latest state and a slow
 * subscriber cannot grow terminal retention without a limit.</p>
 */
public final class ZLinkStatusPublisher<T>
    implements Flow.Publisher<ZLinkObservedStatus<T>> {
    private static final Object SINGLE_SOURCE = new Object();
    private final Supplier<T> snapshot;
    private final Function<T, Object> fingerprint;
    private final Function<T, Object> sourceKey;
    private final int capacity;
    private final Predicate<T> terminal;
    private final Predicate<T> preserve;
    private final Executor dispatcher;
    private final CopyOnWriteArrayList<SnapshotSubscription> subscriptions =
        new CopyOnWriteArrayList<>();
    private final AtomicBoolean workPending = new AtomicBoolean();
    private final AtomicBoolean observationPending = new AtomicBoolean();
    private final AtomicBoolean drainScheduled = new AtomicBoolean();
    private final Object retentionGate = new Object();
    private Consumer<Boolean> retention;
    private int activeSubscriptions;

    private ZLinkStatusPublisher(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        Function<T, Object> sourceKey,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve,
        Executor dispatcher) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("capacity must be positive");
        }
        this.snapshot = Objects.requireNonNull(snapshot, "snapshot");
        this.fingerprint = Objects.requireNonNull(fingerprint, "fingerprint");
        this.sourceKey = Objects.requireNonNull(sourceKey, "sourceKey");
        this.capacity = capacity;
        this.terminal = Objects.requireNonNull(terminal, "terminal");
        this.preserve = Objects.requireNonNull(preserve, "preserve");
        this.dispatcher = Objects.requireNonNull(dispatcher, "dispatcher");
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity) {
        return create(
            snapshot,
            fingerprint,
            ignored -> SINGLE_SOURCE,
            capacity,
            ignored -> false,
            ignored -> false,
            ForkJoinPool.commonPool());
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity,
        Predicate<T> terminal) {
        return create(
            snapshot,
            fingerprint,
            ignored -> SINGLE_SOURCE,
            capacity,
            terminal,
            ignored -> false,
            ForkJoinPool.commonPool());
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve) {
        return create(
            snapshot,
            fingerprint,
            ignored -> SINGLE_SOURCE,
            capacity,
            terminal,
            preserve,
            ForkJoinPool.commonPool());
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        Function<T, Object> sourceKey,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve) {
        return create(
            snapshot,
            fingerprint,
            sourceKey,
            capacity,
            terminal,
            preserve,
            ForkJoinPool.commonPool());
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        Function<T, Object> sourceKey,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve,
        Executor dispatcher) {
        return new ZLinkStatusPublisher<>(
            snapshot,
            fingerprint,
            sourceKey,
            capacity,
            terminal,
            preserve,
            dispatcher);
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve,
        Executor dispatcher) {
        return create(
            snapshot,
            fingerprint,
            ignored -> SINGLE_SOURCE,
            capacity,
            terminal,
            preserve,
            dispatcher);
    }

    /** Signals that the source may have a new snapshot. */
    public void signal() {
        observationPending.set(true);
        schedule();
    }

    /**
     * Registers the owner callback that keeps this publisher reachable while a
     * subscription is live. It is called with {@code true} when the first
     * subscription is accepted and with {@code false} once the last one is
     * cancelled or failed.
     *
     * <p>A signal source may only hold a publisher weakly, because a publisher
     * that is never subscribed has to stay collectable. A subscriber that drops
     * its {@link Flow.Subscription} is the natural call shape, so the
     * subscription itself cannot be the only strong reference either. This
     * callback closes that gap without turning an unsubscribed publisher into a
     * leak.</p>
     */
    public void onActiveSubscriptions(Consumer<Boolean> listener) {
        Objects.requireNonNull(listener, "listener");
        boolean active;
        synchronized (retentionGate) {
            if (retention != null) {
                throw new IllegalStateException(
                    "an active subscription listener is already registered");
            }
            retention = listener;
            active = activeSubscriptions > 0;
        }
        listener.accept(active);
    }

    @Override
    public void subscribe(
        Flow.Subscriber<? super ZLinkObservedStatus<T>> subscriber) {
        Objects.requireNonNull(subscriber, "subscriber");
        SnapshotSubscription subscription = new SnapshotSubscription(subscriber);
        subscriber.onSubscribe(subscription);
        if (subscription.cancelled.get()) {
            return;
        }
        subscriptions.add(subscription);
        retainForSubscription();
        if (subscription.cancelled.get()
            && subscriptions.remove(subscription)) {
            releaseForSubscription();
            return;
        }
        signal();
    }

    private void retainForSubscription() {
        Consumer<Boolean> listener;
        synchronized (retentionGate) {
            listener = ++activeSubscriptions == 1 ? retention : null;
        }
        if (listener != null) {
            listener.accept(true);
        }
    }

    private void releaseForSubscription() {
        Consumer<Boolean> listener;
        synchronized (retentionGate) {
            listener = --activeSubscriptions == 0 ? retention : null;
        }
        if (listener != null) {
            listener.accept(false);
        }
    }

    private void schedule() {
        workPending.set(true);
        if (drainScheduled.compareAndSet(false, true)) {
            try {
                dispatcher.execute(this::drain);
            } catch (RuntimeException failure) {
                drainScheduled.set(false);
                subscriptions.forEach(subscription -> subscription.fail(failure));
                throw failure;
            }
        }
    }

    private void drain() {
        try {
            while (workPending.getAndSet(false)) {
                if (observationPending.getAndSet(false)) {
                    T current = snapshot.get();
                    Object currentFingerprint = fingerprint.apply(current);
                    for (SnapshotSubscription subscription : subscriptions) {
                        subscription.observe(current, currentFingerprint);
                    }
                }
                for (SnapshotSubscription subscription : subscriptions) {
                    subscription.scheduleDelivery();
                }
            }
        } catch (Throwable failure) {
            subscriptions.forEach(subscription -> subscription.fail(failure));
        } finally {
            drainScheduled.set(false);
            if (workPending.get()
                && drainScheduled.compareAndSet(false, true)) {
                dispatcher.execute(this::drain);
            }
        }
    }

    private final class SnapshotSubscription implements Flow.Subscription {
        private final Flow.Subscriber<? super ZLinkObservedStatus<T>> subscriber;
        private final AtomicLong demand = new AtomicLong();
        private final AtomicBoolean cancelled = new AtomicBoolean();
        private final AtomicBoolean deliveryWorkPending = new AtomicBoolean();
        private final AtomicBoolean deliveryScheduled = new AtomicBoolean();
        private final Object monitor = new Object();
        private final LinkedHashMap<Object, Pending<T>> latestBySource =
            new LinkedHashMap<>();
        private final ArrayDeque<Pending<T>> retained = new ArrayDeque<>();
        private final Map<Object, Object> previousFingerprints =
            new HashMap<>();
        private final Set<Object> pendingTerminals = new HashSet<>();
        private long coalescedCount;
        private long discardedTerminalCount;

        SnapshotSubscription(
            Flow.Subscriber<? super ZLinkObservedStatus<T>> subscriber) {
            this.subscriber = subscriber;
        }

        @Override
        public void request(long count) {
            if (count <= 0) {
                cancel();
                subscriber.onError(new IllegalArgumentException(
                    "subscription demand must be positive"));
                return;
            }
            demand.getAndUpdate(current -> {
                long next = current + count;
                return next < 0 ? Long.MAX_VALUE : next;
            });
            scheduleDelivery();
        }

        @Override
        public void cancel() {
            if (cancelled.compareAndSet(false, true)
                && subscriptions.remove(this)) {
                releaseForSubscription();
            }
        }

        private void observe(T value, Object currentFingerprint) {
            Object currentSourceKey = Objects.requireNonNull(
                sourceKey.apply(value),
                "sourceKey returned null");
            synchronized (monitor) {
                if (cancelled.get()
                    || pendingTerminals.contains(currentSourceKey)
                    || (previousFingerprints.containsKey(currentSourceKey)
                        && Objects.equals(
                            previousFingerprints.get(currentSourceKey),
                            currentFingerprint))) {
                    return;
                }
                previousFingerprints.put(
                    currentSourceKey,
                    currentFingerprint);
                enqueue(
                    currentSourceKey,
                    value,
                    terminal.test(value),
                    preserve.test(value));
            }
        }

        private void scheduleDelivery() {
            deliveryWorkPending.set(true);
            if (deliveryScheduled.compareAndSet(false, true)) {
                try {
                    dispatcher.execute(this::drainDelivery);
                } catch (RuntimeException failure) {
                    deliveryScheduled.set(false);
                    fail(failure);
                }
            }
        }

        private void drainDelivery() {
            try {
                while (deliveryWorkPending.getAndSet(false)) {
                    deliverAvailable();
                }
            } finally {
                deliveryScheduled.set(false);
                if (deliveryWorkPending.get()
                    && deliveryScheduled.compareAndSet(false, true)) {
                    dispatcher.execute(this::drainDelivery);
                }
            }
        }

        private void deliverAvailable() {
            while (!cancelled.get() && demand.get() > 0) {
                ZLinkObservedStatus<T> observedStatus;
                Pending<T> next;
                synchronized (monitor) {
                    next = retained.pollFirst();
                    if (next == null && !latestBySource.isEmpty()) {
                        var iterator = latestBySource.entrySet().iterator();
                        next = iterator.next().getValue();
                        iterator.remove();
                    }
                    if (next == null) {
                        return;
                    }
                    observedStatus = new ZLinkObservedStatus<>(
                        next.value(),
                        new ZLinkObservationLoss(
                            coalescedCount,
                            discardedTerminalCount));
                }
                if (demand.get() != Long.MAX_VALUE) {
                    demand.decrementAndGet();
                }
                try {
                    subscriber.onNext(observedStatus);
                    if (next.terminal()) {
                        synchronized (monitor) {
                            releaseSource(next.sourceKey());
                        }
                    }
                } catch (Throwable failure) {
                    fail(failure);
                    return;
                }
            }
        }

        private void fail(Throwable failure) {
            if (cancelled.compareAndSet(false, true)) {
                if (subscriptions.remove(this)) {
                    releaseForSubscription();
                }
                subscriber.onError(failure);
            }
        }

        private void enqueue(
            Object currentSourceKey,
            T value,
            boolean isTerminal,
            boolean isPreserved) {
            if (latestBySource.remove(currentSourceKey) != null) {
                coalescedCount = saturatingIncrement(coalescedCount);
            }

            if (isTerminal || isPreserved) {
                if (retained.size() >= capacity) {
                    Pending<T> discarded = retained.removeFirst();
                    discardedTerminalCount = saturatingIncrement(
                        discardedTerminalCount);
                    if (discarded.terminal()) {
                        releaseSource(discarded.sourceKey());
                    }
                }
                retained.addLast(new Pending<>(
                    currentSourceKey,
                    value,
                    isTerminal));
                if (isTerminal) {
                    pendingTerminals.add(currentSourceKey);
                }
                return;
            }

            latestBySource.put(
                currentSourceKey,
                new Pending<>(currentSourceKey, value, false));
        }

        private void releaseSource(Object releasedSourceKey) {
            latestBySource.remove(releasedSourceKey);
            previousFingerprints.remove(releasedSourceKey);
            pendingTerminals.remove(releasedSourceKey);
        }
    }

    private record Pending<T>(Object sourceKey, T value, boolean terminal) {
    }

    private static long saturatingIncrement(long value) {
        return value == Long.MAX_VALUE ? value : value + 1;
    }
}
