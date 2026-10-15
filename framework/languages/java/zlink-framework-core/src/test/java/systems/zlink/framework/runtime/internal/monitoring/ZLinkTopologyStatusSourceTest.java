package systems.zlink.framework.runtime.internal.monitoring;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.monitoring.ZLinkObservedStatus;
import systems.zlink.framework.monitoring.ZLinkTopologyState;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.Flow;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.stream.IntStream;

final class ZLinkTopologyStatusSourceTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void snapshotCanQueryAnOwnerThatAlsoReadsTheSource(boolean alreadyPublished) throws Exception {
        var ownerThread = new AtomicReference<Thread>();
        try (var owner =
                Executors.newSingleThreadExecutor(
                        task -> {
                            Thread thread = new Thread(task);
                            ownerThread.set(thread);
                            return thread;
                        })) {
            var sourceRef = new AtomicReference<ZLinkTopologyStatusSource<Status>>();
            var source =
                    new ZLinkTopologyStatusSource<Status>(
                            (key, previous) -> {
                                if (Thread.currentThread() == ownerThread.get()) {
                                    return new Status(key, ZLinkTopologyState.READY, 0);
                                }
                                try {
                                    return owner.submit(() -> sourceRef.get().snapshot(key))
                                            .get(1, TimeUnit.SECONDS);
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                    throw new CompletionException(interrupted);
                                } catch (ExecutionException | TimeoutException failure) {
                                    throw new CompletionException(failure);
                                }
                            },
                            Status::state,
                            (status, sequence) ->
                                    new Status(status.key(), status.state(), sequence),
                            Status::sequence,
                            status -> status.state() == ZLinkTopologyState.STOPPED,
                            status -> status.state() == ZLinkTopologyState.STOPPING);
            sourceRef.set(source);
            if (alreadyPublished) {
                assertEquals(
                        1,
                        owner.submit(() -> source.snapshot("mesh"))
                                .get(1, TimeUnit.SECONDS)
                                .sequence());
            }
            assertEquals(1, source.snapshot("mesh").sequence());
        }
    }

    @Test
    void staleProjectionCannotReplaceAnAlreadyPublishedStatus() throws Exception {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var pausedReader = new AtomicReference<Thread>();
        var captured = new CountDownLatch(1);
        var release = new CompletableFuture<Void>();
        var source =
                new ZLinkTopologyStatusSource<Status>(
                        (key, previous) -> {
                            var projected = new Status(key, state.get(), 0);
                            if (Thread.currentThread() == pausedReader.get()) {
                                captured.countDown();
                                try {
                                    release.get(1, TimeUnit.SECONDS);
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                    throw new CompletionException(interrupted);
                                } catch (ExecutionException | TimeoutException failure) {
                                    throw new CompletionException(failure);
                                }
                            }
                            return projected;
                        },
                        Status::state,
                        (status, sequence) -> new Status(status.key(), status.state(), sequence),
                        Status::sequence,
                        status -> status.state() == ZLinkTopologyState.STOPPED,
                        status -> status.state() == ZLinkTopologyState.STOPPING);
        source.snapshot("mesh");
        try (var reader = Executors.newSingleThreadExecutor()) {
            var stale =
                    reader.submit(
                            () -> {
                                pausedReader.set(Thread.currentThread());
                                return source.snapshot("mesh");
                            });
            try {
                org.junit.jupiter.api.Assertions.assertTrue(captured.await(1, TimeUnit.SECONDS));
                state.set(ZLinkTopologyState.STOPPING);
                var current = source.snapshot("mesh");
                assertEquals(2, current.sequence());
                release.complete(null);
                assertEquals(current, stale.get(1, TimeUnit.SECONDS));
                assertEquals(current, source.snapshot("mesh"));
            } finally {
                release.complete(null);
            }
        }
    }

    @Test
    void firstQueryPublishesSequenceOneWithoutObservers() {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var source = source(state);
        assertEquals(1, source.snapshot("first").sequence());
        assertEquals(1, source.snapshot("first").sequence());
        assertEquals(1, source.snapshot("other").sequence());
    }

    @Test
    void changedQueryPublishesNextSequenceWithoutObservers() {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var source = source(state);
        var initial = source.snapshot("first");
        state.set(ZLinkTopologyState.STOPPING);
        var changed = source.snapshot("first");
        assertEquals(initial.sequence() + 1, changed.sequence());
        assertEquals(changed, source.snapshot("first"));
    }

    @Test
    void initialTerminalQueryIsPublishedAndCannotRegress() throws Exception {
        var state = new AtomicReference<>(ZLinkTopologyState.STOPPED);
        var source = source(state);
        var terminal = source.snapshot("first");
        assertEquals(1, terminal.sequence());
        state.set(ZLinkTopologyState.READY);
        assertEquals(terminal, source.snapshot("first"));
        assertEquals(terminal, first(source.observe("first", 1)));
    }

    @Test
    void queriedChangeAndObserversShareOnePublishedSequence() throws Exception {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var source = source(state);
        var initial = source.snapshot("first");
        assertEquals(initial, first(source.observe("first", 1)));
        state.set(ZLinkTopologyState.STOPPING);
        var changed = source.snapshot("first");
        assertEquals(initial.sequence() + 1, changed.sequence());
        assertEquals(changed, first(source.observe("first", 1)));
        assertEquals(changed, first(source.observe("first", 8)));
        assertEquals(changed, source.snapshot("first"));
    }

    @Test
    void signalsPublishChangedPayloadAfterLastObserverCancels() throws Exception {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var read = new AtomicReference<CountDownLatch>();
        var source =
                new ZLinkTopologyStatusSource<Status>(
                        (key, previous) -> {
                            var barrier = read.get();
                            if (barrier != null) {
                                barrier.countDown();
                            }
                            return new Status(key, state.get(), 0);
                        },
                        Status::state,
                        (status, sequence) -> new Status(status.key(), status.state(), sequence),
                        Status::sequence,
                        status -> status.state() == ZLinkTopologyState.STOPPED,
                        status -> status.state() == ZLinkTopologyState.STOPPING);
        var initial = first(source.observe("first", 1));
        state.set(ZLinkTopologyState.STOPPING);
        var signaledRead = new CountDownLatch(1);
        read.set(signaledRead);
        source.signal("first");
        org.junit.jupiter.api.Assertions.assertTrue(signaledRead.await(1, TimeUnit.SECONDS));
        var changed = source.snapshot("first");
        assertEquals(initial.sequence() + 1, changed.sequence());
        state.set(ZLinkTopologyState.STOPPED);
        var broadcastRead = new CountDownLatch(1);
        read.set(broadcastRead);
        source.signalAll();
        org.junit.jupiter.api.Assertions.assertTrue(broadcastRead.await(1, TimeUnit.SECONDS));
        assertEquals(changed.sequence() + 1, source.snapshot("first").sequence());
    }

    @Test
    void concurrentQueriesPublishOneSequenceForTheSameChangedPayload() {
        var state = new AtomicReference<>(ZLinkTopologyState.READY);
        var source = source(state);
        var initial = source.snapshot("first");
        state.set(ZLinkTopologyState.STOPPING);
        var queries =
                IntStream.range(0, 16)
                        .mapToObj(
                                ignored ->
                                        CompletableFuture.supplyAsync(
                                                () -> source.snapshot("first")))
                        .toList();
        for (var query : queries) {
            assertEquals(initial.sequence() + 1, query.join().sequence());
        }
    }

    @Test
    void registrationFailureAfterFirstSubscriptionReportsCauseAndAllowsNextSubscriber()
            throws Exception {
        var source = source(new AtomicReference<>(ZLinkTopologyState.READY));
        var publisher = source.observe("first", 1);
        var failure = new IllegalStateException("registration failed");
        var registrations = new java.util.concurrent.atomic.AtomicInteger();
        source.onActiveSubscriptions(
                "first",
                active -> {
                    if (active && registrations.incrementAndGet() == 1)
                        source.fail("first", failure);
                });
        assertEquals(0, registrations.get());
        var errors = new LinkedBlockingQueue<Throwable>();
        var deliveries = new java.util.concurrent.atomic.AtomicInteger();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    public void onSubscribe(Flow.Subscription subscription) {
                        subscription.request(1);
                    }

                    public void onNext(ZLinkObservedStatus<Status> status) {
                        deliveries.incrementAndGet();
                    }

                    public void onError(Throwable cause) {
                        errors.add(cause);
                    }

                    public void onComplete() {}
                });
        org.junit.jupiter.api.Assertions.assertSame(failure, errors.poll(1, TimeUnit.SECONDS));
        assertEquals(0, deliveries.get());
        assertEquals(0, errors.size());
        assertNotNull(first(publisher));
        assertEquals(2, registrations.get());
    }

    private static ZLinkTopologyStatusSource<Status> source(
            AtomicReference<ZLinkTopologyState> state) {
        return new ZLinkTopologyStatusSource<>(
                (key, previous) -> new Status(key, state.get(), 0),
                Status::state,
                (status, sequence) -> new Status(status.key(), status.state(), sequence),
                Status::sequence,
                status -> status.state() == ZLinkTopologyState.STOPPED,
                status -> status.state() == ZLinkTopologyState.STOPPING);
    }

    private static Status first(Flow.Publisher<ZLinkObservedStatus<Status>> publisher)
            throws Exception {
        var received = new LinkedBlockingQueue<Status>();
        publisher.subscribe(
                new Flow.Subscriber<>() {
                    private Flow.Subscription subscription;

                    @Override
                    public void onSubscribe(Flow.Subscription value) {
                        subscription = value;
                        value.request(1);
                    }

                    @Override
                    public void onNext(ZLinkObservedStatus<Status> value) {
                        subscription.cancel();
                        received.add(value.status());
                    }

                    @Override
                    public void onError(Throwable failure) {
                        throw new AssertionError(failure);
                    }

                    @Override
                    public void onComplete() {}
                });
        Status status = received.poll(1, TimeUnit.SECONDS);
        assertNotNull(status);
        return status;
    }

    private record Status(String key, ZLinkTopologyState state, long sequence) {}
}
