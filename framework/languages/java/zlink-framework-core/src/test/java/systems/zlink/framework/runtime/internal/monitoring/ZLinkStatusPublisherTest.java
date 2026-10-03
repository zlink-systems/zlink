package systems.zlink.framework.runtime.internal.monitoring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.monitoring.ZLinkObservedStatus;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.IntConsumer;

final class ZLinkStatusPublisherTest {
    @Test
    void subscriptionAcceptedDuringFailureDetachRechecksSourceRetention() throws Exception {
        var publisher =
                ZLinkStatusPublisher.create(
                        () -> 1,
                        value -> value,
                        1,
                        ignored -> false,
                        ignored -> false,
                        Runnable::run);
        var registrations = new AtomicInteger();
        publisher.onActiveSubscriptions(
                active -> {
                    if (active) registrations.incrementAndGet();
                });
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription subscription) {}

                    public void onNext(ZLinkObservedStatus<Integer> value) {}

                    public void onError(Throwable failure) {}

                    public void onComplete() {}
                });
        var gateField = ZLinkStatusPublisher.class.getDeclaredField("retentionGate");
        gateField.setAccessible(true);
        var listField = ZLinkStatusPublisher.class.getDeclaredField("subscriptions");
        listField.setAccessible(true);
        var current = (java.util.List<?>) listField.get(publisher);
        CompletableFuture<Void> failed;
        var replacement = new CompletableFuture<Flow.Subscription>();
        synchronized (gateField.get(publisher)) {
            failed =
                    CompletableFuture.runAsync(
                            () -> publisher.fail(new IllegalStateException("registration")));
            long limit = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
            while (!current.isEmpty() && System.nanoTime() < limit) Thread.onSpinWait();
            assertTrue(current.isEmpty());
            publisher.subscribe(statusSubscriber(new CopyOnWriteArrayList<>(), replacement));
            assertEquals(1, registrations.get());
        }
        failed.get(3, TimeUnit.SECONDS);
        assertEquals(2, registrations.get());
        replacement.join().cancel();
    }

    @Test
    void sourceFailureDetachesEveryCurrentObserverBeforeImmediateResubscribe() {
        var publisher =
                ZLinkStatusPublisher.create(
                        () -> 1,
                        value -> value,
                        1,
                        ignored -> false,
                        ignored -> false,
                        Runnable::run);
        var registrations = new AtomicInteger();
        publisher.onActiveSubscriptions(
                active -> {
                    if (active) registrations.incrementAndGet();
                });
        var cause = new IllegalStateException("source registration failed");
        var errors = new CopyOnWriteArrayList<Throwable>();
        var replacement = new CompletableFuture<Flow.Subscription>();
        var replacementValues = new CopyOnWriteArrayList<ZLinkObservedStatus<Integer>>();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription subscription) {}

                    public void onNext(ZLinkObservedStatus<Integer> value) {}

                    public void onError(Throwable failure) {
                        errors.add(failure);
                        publisher.subscribe(statusSubscriber(replacementValues, replacement));
                    }

                    public void onComplete() {}
                });
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription subscription) {}

                    public void onNext(ZLinkObservedStatus<Integer> value) {}

                    public void onError(Throwable failure) {
                        errors.add(failure);
                    }

                    public void onComplete() {}
                });
        assertEquals(1, registrations.get());
        publisher.fail(cause);
        assertEquals(List.of(cause, cause), errors);
        assertEquals(2, registrations.get());
        replacement.join().request(1);
        assertEquals(1, replacementValues.size());
        replacement.join().cancel();
    }

    @Test
    void throwingErrorObserverDoesNotSkipOtherObserverAndLogsOriginalCallbackCause() {
        var publisher =
                ZLinkStatusPublisher.create(
                        () -> 1,
                        value -> value,
                        1,
                        ignored -> false,
                        ignored -> false,
                        Runnable::run);
        var cause = new IllegalStateException("source registration failed");
        var thrown = new IllegalArgumentException("observer failed");
        var errors = new CopyOnWriteArrayList<Throwable>();
        var diagnostics = new CopyOnWriteArrayList<Throwable>();
        var logger = java.util.logging.Logger.getLogger(ZLinkStatusPublisher.class.getName());
        var handler =
                new java.util.logging.Handler() {
                    public void publish(java.util.logging.LogRecord record) {
                        diagnostics.add(record.getThrown());
                    }

                    public void flush() {}

                    public void close() {}
                };
        logger.addHandler(handler);
        try {
            publisher.subscribe(
                    new Flow.Subscriber<>() {
                        public void onSubscribe(Flow.Subscription subscription) {}

                        public void onNext(ZLinkObservedStatus<Integer> value) {}

                        public void onError(Throwable failure) {
                            errors.add(failure);
                            throw thrown;
                        }

                        public void onComplete() {}
                    });
            publisher.subscribe(
                    new Flow.Subscriber<>() {
                        public void onSubscribe(Flow.Subscription subscription) {}

                        public void onNext(ZLinkObservedStatus<Integer> value) {}

                        public void onError(Throwable failure) {
                            errors.add(failure);
                        }

                        public void onComplete() {}
                    });
            org.junit.jupiter.api.Assertions.assertDoesNotThrow(() -> publisher.fail(cause));
            assertEquals(List.of(cause, cause), errors);
            assertEquals(List.of(thrown), diagnostics);
            publisher.fail(cause);
            assertEquals(2, errors.size());
        } finally {
            logger.removeHandler(handler);
        }
    }

    @Test
    void subscriptionCapacitiesKeepTerminalRetentionAndLossIndependent() throws Exception {
        var state = new AtomicReference<>(new SourceStatus("A", 0, false));
        var publisher =
                ZLinkStatusPublisher.create(
                        state::get,
                        SourceStatus::sequence,
                        SourceStatus::source,
                        2,
                        SourceStatus::terminal,
                        ignored -> false,
                        Runnable::run);
        var narrow = new CopyOnWriteArrayList<ZLinkObservedStatus<SourceStatus>>();
        var wide = new CopyOnWriteArrayList<ZLinkObservedStatus<SourceStatus>>();
        var narrowSubscription = new CompletableFuture<Flow.Subscription>();
        var wideSubscription = new CompletableFuture<Flow.Subscription>();
        publisher.subscribe(statusSubscriber(narrow, narrowSubscription), 1);
        publisher.subscribe(statusSubscriber(wide, wideSubscription), 3);
        for (String source : List.of("A", "B", "C")) {
            state.set(new SourceStatus(source, 1, true));
            publisher.signal();
        }
        narrowSubscription.get(1, TimeUnit.SECONDS).request(3);
        wideSubscription.get(1, TimeUnit.SECONDS).request(3);
        assertEquals(List.of("C"), narrow.stream().map(item -> item.status().source()).toList());
        assertEquals(
                List.of("A", "B", "C"), wide.stream().map(item -> item.status().source()).toList());
        assertEquals(2, narrow.getFirst().loss().discardedTerminalCount());
        assertEquals(0, wide.getFirst().loss().discardedTerminalCount());
    }

    @Test
    void sourceSignalsWithoutSubscribersDoNotReadSnapshots() {
        var reads = new AtomicInteger();
        var publisher =
                ZLinkStatusPublisher.create(
                        reads::incrementAndGet,
                        value -> value,
                        2,
                        ignored -> false,
                        ignored -> false,
                        Runnable::run);
        publisher.signalIfSubscribed();
        publisher.signalIfSubscribed();
        assertEquals(0, reads.get());
        var received = new CopyOnWriteArrayList<ZLinkObservedStatus<Integer>>();
        var subscription = new CompletableFuture<Flow.Subscription>();
        publisher.subscribe(statusSubscriber(received, subscription));
        assertEquals(1, reads.get());
        subscription.join().cancel();
        publisher.signalIfSubscribed();
        assertEquals(1, reads.get());
    }

    @Test
    void intermediateSnapshotsCoalesceOnlyWithinTheSameSource() throws Exception {
        AtomicReference<SourceStatus> state =
                new AtomicReference<>(new SourceStatus("A", 0, false));
        ZLinkStatusPublisher<SourceStatus> publisher =
                ZLinkStatusPublisher.create(
                        state::get,
                        SourceStatus::sequence,
                        SourceStatus::source,
                        2,
                        SourceStatus::terminal,
                        ignored -> false,
                        Runnable::run);
        CompletableFuture<Flow.Subscription> subscribed = new CompletableFuture<>();
        CopyOnWriteArrayList<ZLinkObservedStatus<SourceStatus>> received =
                new CopyOnWriteArrayList<>();
        publisher.subscribe(statusSubscriber(received, subscribed));

        state.set(new SourceStatus("A", 1, false));
        publisher.signal();
        state.set(new SourceStatus("B", 1, false));
        publisher.signal();
        state.set(new SourceStatus("A", 2, false));
        publisher.signal();
        subscribed.get(1, TimeUnit.SECONDS).request(2);

        assertEquals(2, received.size());
        assertTrue(
                received.stream()
                        .anyMatch(
                                item ->
                                        item.status().source().equals("A")
                                                && item.status().sequence() == 2));
        assertTrue(
                received.stream()
                        .anyMatch(
                                item ->
                                        item.status().source().equals("B")
                                                && item.status().sequence() == 1));
        assertEquals(2, received.get(0).loss().coalescedCount());
        assertEquals(0, received.get(0).loss().discardedTerminalCount());
    }

    @Test
    void terminalCapacityDiscardsOnlyTheOldestTerminalAndReleasesItsSource() throws Exception {
        AtomicReference<SourceStatus> state =
                new AtomicReference<>(new SourceStatus("A", 0, false));
        ZLinkStatusPublisher<SourceStatus> publisher =
                ZLinkStatusPublisher.create(
                        state::get,
                        SourceStatus::sequence,
                        SourceStatus::source,
                        2,
                        SourceStatus::terminal,
                        ignored -> false,
                        Runnable::run);
        CompletableFuture<Flow.Subscription> subscribed = new CompletableFuture<>();
        CopyOnWriteArrayList<ZLinkObservedStatus<SourceStatus>> received =
                new CopyOnWriteArrayList<>();
        publisher.subscribe(statusSubscriber(received, subscribed));

        state.set(new SourceStatus("A", 1, true));
        publisher.signal();
        state.set(new SourceStatus("B", 1, true));
        publisher.signal();
        state.set(new SourceStatus("C", 1, true));
        publisher.signal();
        subscribed.get(1, TimeUnit.SECONDS).request(2);

        assertEquals(
                List.of("B", "C"), received.stream().map(item -> item.status().source()).toList());
        assertEquals(1, received.get(0).loss().discardedTerminalCount());

        state.set(new SourceStatus("B", 0, false));
        publisher.signal();
        subscribed.get(1, TimeUnit.SECONDS).request(1);
        assertEquals(
                new SourceStatus("B", 0, false),
                received.get(2).status(),
                "delivering a terminal removes the source key for a new lifecycle");
    }

    @Test
    void preservedMilestoneIsDeliveredBeforeLaterTerminalSnapshot() throws Exception {
        AtomicInteger state = new AtomicInteger();
        ZLinkStatusPublisher<Integer> publisher =
                ZLinkStatusPublisher.create(
                        state::get, value -> value, 4, value -> value == 3, value -> value == 2);
        CopyOnWriteArrayList<Integer> received = new CopyOnWriteArrayList<>();
        CompletableFuture<Flow.Subscription> subscribed = new CompletableFuture<>();
        CompletableFuture<Void> failed = new CompletableFuture<>();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    @Override
                    public void onSubscribe(Flow.Subscription subscription) {
                        subscribed.complete(subscription);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<Integer> item) {
                        received.add(item.status());
                    }

                    @Override
                    public void onError(Throwable failure) {
                        failed.completeExceptionally(failure);
                    }

                    @Override
                    public void onComplete() {
                        failed.completeExceptionally(
                                new AssertionError(
                                        "terminal status must not complete observation"));
                    }
                });
        Flow.Subscription subscription = subscribed.get(1, TimeUnit.SECONDS);
        subscription.request(1);
        long firstDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (received.isEmpty() && System.nanoTime() < firstDeadline) {
            Thread.sleep(1);
        }
        state.set(2);
        publisher.signal();
        Thread.sleep(60);
        state.set(3);
        publisher.signal();
        Thread.sleep(60);
        subscription.request(2);

        long terminalDeadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (received.size() < 3 && System.nanoTime() < terminalDeadline) {
            Thread.sleep(1);
        }
        assertEquals(List.of(0, 2, 3), received);
        assertTrue(!failed.isDone());
    }

    @Test
    void slowObserverDoesNotDelayAnotherObserverAndTerminalIsDelivered() throws Exception {
        AtomicInteger state = new AtomicInteger();
        ZLinkStatusPublisher<Integer> publisher =
                ZLinkStatusPublisher.create(state::get, value -> value, 4, value -> value == 2);
        CountDownLatch slowEntered = new CountDownLatch(1);
        CountDownLatch releaseSlow = new CountDownLatch(1);
        CompletableFuture<Integer> fastTerminal = new CompletableFuture<>();

        publisher.subscribe(
                subscriber(
                        value -> {
                            slowEntered.countDown();
                            try {
                                releaseSlow.await(2, TimeUnit.SECONDS);
                            } catch (InterruptedException failure) {
                                Thread.currentThread().interrupt();
                            }
                        },
                        new CompletableFuture<>()));
        publisher.subscribe(
                subscriber(
                        value -> {
                            if (value == 2) {
                                fastTerminal.complete(value);
                            }
                        },
                        new CompletableFuture<>()));

        assertTrue(slowEntered.await(1, TimeUnit.SECONDS));
        state.set(2);
        publisher.signal();
        assertEquals(2, fastTerminal.get(1, TimeUnit.SECONDS));
        releaseSlow.countDown();
    }

    @Test
    void activeSubscriptionRetentionIsRaisedOnceAndReleasedOnce() throws Exception {
        ZLinkStatusPublisher<Integer> publisher =
                ZLinkStatusPublisher.create(() -> 1, value -> value, 4);
        CopyOnWriteArrayList<Boolean> retention = new CopyOnWriteArrayList<>();
        publisher.onActiveSubscriptions(retention::add);
        assertEquals(List.of(false), retention, "an unsubscribed publisher stays collectable");

        CompletableFuture<Flow.Subscription> first = new CompletableFuture<>();
        CompletableFuture<Flow.Subscription> second = new CompletableFuture<>();
        publisher.subscribe(capturing(first));
        publisher.subscribe(capturing(second));
        assertEquals(
                List.of(false, true), retention, "only the first subscription raises retention");

        first.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(
                List.of(false, true),
                retention,
                "retention survives while another subscription is live");
        second.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(List.of(false, true, false), retention);

        first.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(
                List.of(false, true, false),
                retention,
                "a repeated cancel does not unbalance retention");
    }

    @Test
    void cancellingInsideOnSubscribeLeavesRetentionReleased() {
        ZLinkStatusPublisher<Integer> publisher =
                ZLinkStatusPublisher.create(() -> 1, value -> value, 4);
        CopyOnWriteArrayList<Boolean> retention = new CopyOnWriteArrayList<>();
        publisher.onActiveSubscriptions(retention::add);

        publisher.subscribe(
                new Flow.Subscriber<>() {
                    @Override
                    public void onSubscribe(Flow.Subscription subscription) {
                        subscription.cancel();
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<Integer> item) {}

                    @Override
                    public void onError(Throwable failure) {}

                    @Override
                    public void onComplete() {}
                });

        assertEquals(
                List.of(false),
                retention,
                "a subscriber that cancels immediately never retains the publisher");
    }

    private static Flow.Subscriber<ZLinkObservedStatus<Integer>> capturing(
            CompletableFuture<Flow.Subscription> subscribed) {
        return new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(Flow.Subscription subscription) {
                subscribed.complete(subscription);
            }

            @Override
            public void onNext(ZLinkObservedStatus<Integer> item) {}

            @Override
            public void onError(Throwable failure) {}

            @Override
            public void onComplete() {}
        };
    }

    private static Flow.Subscriber<ZLinkObservedStatus<Integer>> subscriber(
            IntConsumer onNext, CompletableFuture<Void> completed) {
        return new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(Flow.Subscription subscription) {
                subscription.request(Long.MAX_VALUE);
            }

            @Override
            public void onNext(ZLinkObservedStatus<Integer> item) {
                onNext.accept(item.status());
            }

            @Override
            public void onError(Throwable failure) {
                completed.completeExceptionally(failure);
            }

            @Override
            public void onComplete() {}
        };
    }

    private static <T> Flow.Subscriber<ZLinkObservedStatus<T>> statusSubscriber(
            CopyOnWriteArrayList<ZLinkObservedStatus<T>> received,
            CompletableFuture<Flow.Subscription> subscribed) {
        return new Flow.Subscriber<>() {
            @Override
            public void onSubscribe(Flow.Subscription subscription) {
                subscribed.complete(subscription);
            }

            @Override
            public void onNext(ZLinkObservedStatus<T> item) {
                received.add(item);
            }

            @Override
            public void onError(Throwable failure) {
                throw new AssertionError(failure);
            }

            @Override
            public void onComplete() {}
        };
    }

    private record SourceStatus(String source, long sequence, boolean terminal) {}
}
