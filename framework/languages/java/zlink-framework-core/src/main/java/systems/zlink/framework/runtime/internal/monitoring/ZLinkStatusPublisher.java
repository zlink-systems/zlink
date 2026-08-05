package systems.zlink.framework.runtime.internal.monitoring;

import java.util.ArrayDeque;
import java.util.Objects;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.Executor;
import java.util.concurrent.Flow;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicLong;
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
 * Intermediate snapshots are coalesced independently for each subscriber,
 * while preserved milestones and terminal snapshots remain observable.</p>
 */
public final class ZLinkStatusPublisher<T>
    implements Flow.Publisher<ZLinkObservedStatus<T>> {
    private final Supplier<T> snapshot;
    private final Function<T, Object> fingerprint;
    private final int capacity;
    private final Predicate<T> terminal;
    private final Predicate<T> preserve;
    private final Executor dispatcher;
    private final CopyOnWriteArrayList<SnapshotSubscription> subscriptions =
        new CopyOnWriteArrayList<>();
    private final AtomicBoolean workPending = new AtomicBoolean();
    private final AtomicBoolean observationPending = new AtomicBoolean();
    private final AtomicBoolean drainScheduled = new AtomicBoolean();

    private ZLinkStatusPublisher(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve,
        Executor dispatcher) {
        if (capacity <= 0) {
            throw new IllegalArgumentException("capacity must be positive");
        }
        this.snapshot = Objects.requireNonNull(snapshot, "snapshot");
        this.fingerprint = Objects.requireNonNull(fingerprint, "fingerprint");
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
            capacity,
            terminal,
            preserve,
            ForkJoinPool.commonPool());
    }

    public static <T> ZLinkStatusPublisher<T> create(
        Supplier<T> snapshot,
        Function<T, Object> fingerprint,
        int capacity,
        Predicate<T> terminal,
        Predicate<T> preserve,
        Executor dispatcher) {
        return new ZLinkStatusPublisher<>(
            snapshot,
            fingerprint,
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

    @Override
    public void subscribe(
        Flow.Subscriber<? super ZLinkObservedStatus<T>> subscriber) {
        Objects.requireNonNull(subscriber, "subscriber");
        SnapshotSubscription subscription = new SnapshotSubscription(subscriber);
        subscriber.onSubscribe(subscription);
        if (!subscription.cancelled.get()) {
            subscriptions.add(subscription);
            signal();
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
        private final ArrayDeque<Pending<T>> pending = new ArrayDeque<>();
        private Object previousFingerprint;
        private boolean observed;
        private boolean terminalSeen;
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
            if (cancelled.compareAndSet(false, true)) {
                subscriptions.remove(this);
            }
        }

        private void observe(T value, Object currentFingerprint) {
            synchronized (monitor) {
                if (cancelled.get()
                    || terminalSeen
                    || (observed
                        && Objects.equals(previousFingerprint, currentFingerprint))) {
                    return;
                }
                observed = true;
                previousFingerprint = currentFingerprint;
                enqueue(value, terminal.test(value), preserve.test(value));
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
                synchronized (monitor) {
                    Pending<T> next = pending.pollFirst();
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
                } catch (Throwable failure) {
                    fail(failure);
                    return;
                }
            }
        }

        private void fail(Throwable failure) {
            if (cancelled.compareAndSet(false, true)) {
                subscriptions.remove(this);
                subscriber.onError(failure);
            }
        }

        private void enqueue(T value, boolean isTerminal, boolean isPreserved) {
            if (isTerminal) {
                terminalSeen = true;
                if (pending.size() >= capacity) {
                    int index = firstPreservedIndex();
                    if (index < 0) {
                        index = 0;
                    }
                    Pending<T> removed = removeAt(index);
                    if (removed.preserved()) {
                        discardedTerminalCount = saturatingIncrement(
                            discardedTerminalCount);
                    } else {
                        coalescedCount = saturatingIncrement(coalescedCount);
                    }
                }
                pending.addLast(new Pending<>(value, true));
                return;
            }

            int latestNonPreserved = lastNonPreservedIndex();
            if (latestNonPreserved >= 0) {
                removeAt(latestNonPreserved);
                coalescedCount = saturatingIncrement(coalescedCount);
                pending.addLast(new Pending<>(value, isPreserved));
                return;
            }
            if (pending.size() >= capacity) {
                Pending<T> removed = removeAt(0);
                if (removed.preserved()) {
                    discardedTerminalCount = saturatingIncrement(
                        discardedTerminalCount);
                }
            }
            pending.addLast(new Pending<>(value, isPreserved));
        }

        private int firstPreservedIndex() {
            int index = 0;
            for (Pending<T> value : pending) {
                if (value.preserved()) {
                    return index;
                }
                index++;
            }
            return -1;
        }

        private int lastNonPreservedIndex() {
            int index = 0;
            int result = -1;
            for (Pending<T> value : pending) {
                if (!value.preserved()) {
                    result = index;
                }
                index++;
            }
            return result;
        }

        private Pending<T> removeAt(int index) {
            ArrayDeque<Pending<T>> reordered = new ArrayDeque<>();
            Pending<T> removed = null;
            int current = 0;
            while (!pending.isEmpty()) {
                Pending<T> value = pending.removeFirst();
                if (current++ == index) {
                    removed = value;
                } else {
                    reordered.addLast(value);
                }
            }
            pending.addAll(reordered);
            return Objects.requireNonNull(removed, "removed pending observation");
        }
    }

    private record Pending<T>(T value, boolean preserved) {
    }

    private static long saturatingIncrement(long value) {
        return value == Long.MAX_VALUE ? value : value + 1;
    }
}
