package systems.zlink.framework.runtime.internal.monitoring;
import java.util.List;
import java.util.function.IntConsumer;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Flow;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CopyOnWriteArrayList;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.monitoring.ZLinkObservedStatus;

final class ZLinkStatusPublisherTest {
    @Test
    void intermediateSnapshotsCoalesceOnlyWithinTheSameSource()
        throws Exception {
        AtomicReference<SourceStatus> state = new AtomicReference<>(
            new SourceStatus("A", 0, false));
        ZLinkStatusPublisher<SourceStatus> publisher = ZLinkStatusPublisher.create(
            state::get,
            SourceStatus::sequence,
            SourceStatus::source,
            2,
            SourceStatus::terminal,
            ignored -> false,
            Runnable::run);
        CompletableFuture<Flow.Subscription> subscribed =
            new CompletableFuture<>();
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
        assertTrue(received.stream().anyMatch(item ->
            item.status().source().equals("A")
                && item.status().sequence() == 2));
        assertTrue(received.stream().anyMatch(item ->
            item.status().source().equals("B")
                && item.status().sequence() == 1));
        assertEquals(2, received.get(0).loss().coalescedCount());
        assertEquals(0, received.get(0).loss().discardedTerminalCount());
    }

    @Test
    void terminalCapacityDiscardsOnlyTheOldestTerminalAndReleasesItsSource()
        throws Exception {
        AtomicReference<SourceStatus> state = new AtomicReference<>(
            new SourceStatus("A", 0, false));
        ZLinkStatusPublisher<SourceStatus> publisher = ZLinkStatusPublisher.create(
            state::get,
            SourceStatus::sequence,
            SourceStatus::source,
            2,
            SourceStatus::terminal,
            ignored -> false,
            Runnable::run);
        CompletableFuture<Flow.Subscription> subscribed =
            new CompletableFuture<>();
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

        assertEquals(List.of("B", "C"), received.stream()
            .map(item -> item.status().source())
            .toList());
        assertEquals(1, received.get(0).loss().discardedTerminalCount());

        state.set(new SourceStatus("B", 0, false));
        publisher.signal();
        subscribed.get(1, TimeUnit.SECONDS).request(1);
        assertEquals(new SourceStatus("B", 0, false), received.get(2).status(),
            "delivering a terminal removes the source key for a new lifecycle");
    }

    @Test
    void preservedMilestoneIsDeliveredBeforeLaterTerminalSnapshot()
        throws Exception {
        AtomicInteger state = new AtomicInteger();
        ZLinkStatusPublisher<Integer> publisher = ZLinkStatusPublisher.create(
            state::get,
            value -> value,
            4,
            value -> value == 3,
            value -> value == 2);
        CopyOnWriteArrayList<Integer> received =
            new CopyOnWriteArrayList<>();
        CompletableFuture<Flow.Subscription> subscribed =
            new CompletableFuture<>();
        CompletableFuture<Void> failed = new CompletableFuture<>();
        publisher.subscribe(new Flow.Subscriber<>() {
            @Override public void onSubscribe(Flow.Subscription subscription) {
                subscribed.complete(subscription);
            }
            @Override public void onNext(ZLinkObservedStatus<Integer> item) {
                received.add(item.status());
            }
            @Override public void onError(Throwable failure) {
                failed.completeExceptionally(failure);
            }
            @Override public void onComplete() { failed.completeExceptionally(
                new AssertionError("terminal status must not complete observation")); }
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
    void slowObserverDoesNotDelayAnotherObserverAndTerminalIsDelivered()
        throws Exception {
        AtomicInteger state = new AtomicInteger();
        ZLinkStatusPublisher<Integer> publisher = ZLinkStatusPublisher.create(
            state::get,
            value -> value,
            4,
            value -> value == 2);
        CountDownLatch slowEntered = new CountDownLatch(1);
        CountDownLatch releaseSlow = new CountDownLatch(1);
        CompletableFuture<Integer> fastTerminal = new CompletableFuture<>();

        publisher.subscribe(subscriber(value -> {
            slowEntered.countDown();
            try {
                releaseSlow.await(2, TimeUnit.SECONDS);
            } catch (InterruptedException failure) {
                Thread.currentThread().interrupt();
            }
        }, new CompletableFuture<>()));
        publisher.subscribe(subscriber(value -> {
            if (value == 2) {
                fastTerminal.complete(value);
            }
        }, new CompletableFuture<>()));

        assertTrue(slowEntered.await(1, TimeUnit.SECONDS));
        state.set(2);
        publisher.signal();
        assertEquals(2, fastTerminal.get(1, TimeUnit.SECONDS));
        releaseSlow.countDown();
    }

    @Test
    void activeSubscriptionRetentionIsRaisedOnceAndReleasedOnce()
        throws Exception {
        ZLinkStatusPublisher<Integer> publisher = ZLinkStatusPublisher.create(
            () -> 1,
            value -> value,
            4);
        CopyOnWriteArrayList<Boolean> retention = new CopyOnWriteArrayList<>();
        publisher.onActiveSubscriptions(retention::add);
        assertEquals(List.of(false), retention,
            "an unsubscribed publisher stays collectable");

        CompletableFuture<Flow.Subscription> first = new CompletableFuture<>();
        CompletableFuture<Flow.Subscription> second = new CompletableFuture<>();
        publisher.subscribe(capturing(first));
        publisher.subscribe(capturing(second));
        assertEquals(List.of(false, true), retention,
            "only the first subscription raises retention");

        first.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(List.of(false, true), retention,
            "retention survives while another subscription is live");
        second.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(List.of(false, true, false), retention);

        first.get(1, TimeUnit.SECONDS).cancel();
        assertEquals(List.of(false, true, false), retention,
            "a repeated cancel does not unbalance retention");
    }

    @Test
    void cancellingInsideOnSubscribeLeavesRetentionReleased() {
        ZLinkStatusPublisher<Integer> publisher = ZLinkStatusPublisher.create(
            () -> 1,
            value -> value,
            4);
        CopyOnWriteArrayList<Boolean> retention = new CopyOnWriteArrayList<>();
        publisher.onActiveSubscriptions(retention::add);

        publisher.subscribe(new Flow.Subscriber<>() {
            @Override public void onSubscribe(Flow.Subscription subscription) {
                subscription.cancel();
            }

            @Override public void onNext(ZLinkObservedStatus<Integer> item) { }

            @Override public void onError(Throwable failure) { }

            @Override public void onComplete() { }
        });

        assertEquals(List.of(false), retention,
            "a subscriber that cancels immediately never retains the publisher");
    }

    private static Flow.Subscriber<ZLinkObservedStatus<Integer>> capturing(
        CompletableFuture<Flow.Subscription> subscribed) {
        return new Flow.Subscriber<>() {
            @Override public void onSubscribe(Flow.Subscription subscription) {
                subscribed.complete(subscription);
            }

            @Override public void onNext(ZLinkObservedStatus<Integer> item) { }

            @Override public void onError(Throwable failure) { }

            @Override public void onComplete() { }
        };
    }

    private static Flow.Subscriber<ZLinkObservedStatus<Integer>> subscriber(
        IntConsumer onNext,
        CompletableFuture<Void> completed) {
        return new Flow.Subscriber<>() {
            @Override public void onSubscribe(Flow.Subscription subscription) {
                subscription.request(Long.MAX_VALUE);
            }

            @Override public void onNext(ZLinkObservedStatus<Integer> item) {
                onNext.accept(item.status());
            }

            @Override public void onError(Throwable failure) {
                completed.completeExceptionally(failure);
            }

            @Override public void onComplete() { }
        };
    }

    private static <T> Flow.Subscriber<ZLinkObservedStatus<T>> statusSubscriber(
        CopyOnWriteArrayList<ZLinkObservedStatus<T>> received,
        CompletableFuture<Flow.Subscription> subscribed) {
        return new Flow.Subscriber<>() {
            @Override public void onSubscribe(Flow.Subscription subscription) {
                subscribed.complete(subscription);
            }

            @Override public void onNext(ZLinkObservedStatus<T> item) {
                received.add(item);
            }

            @Override public void onError(Throwable failure) {
                throw new AssertionError(failure);
            }

            @Override public void onComplete() { }
        };
    }

    private record SourceStatus(String source, long sequence, boolean terminal) {
    }
}
