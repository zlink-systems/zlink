package systems.zlink.framework.runtime.channels;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Future;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.testing.ZLinkLocationStoreTestAdapter;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSocketMonitorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkFanoutLocationRuntimeTest {
    @Test
    void monitorAndStopJoinTheAdmittedSubscriberReceive() throws Exception {
        TestStore store = new TestStore();
        store.rows = List.of(descriptor());
        try (ExecutorService lifecycle = Executors.newFixedThreadPool(2);
             Fixture fixture = new Fixture(store)) {
            fixture.start();
            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            subscriber.blockReceive();
            assertTrue(subscriber.subscribeEntered.await(1, TimeUnit.SECONDS));

            Future<?> monitor = lifecycle.submit(() ->
                subscriber.monitor.emit("DISCONNECTED"));
            monitor.get(1, TimeUnit.SECONDS);
            CountDownLatch stopReserved = new CountDownLatch(1);
            Future<?> stop = lifecycle.submit(() -> {
                CompletionStage<Void> settlement = fixture.runtime.stop();
                stopReserved.countDown();
                settlement.toCompletableFuture().join();
            });

            try {
                assertTrue(stopReserved.await(1, TimeUnit.SECONDS));
                assertFalse(stop.isDone());
                assertEquals(0, subscriber.disconnectCalls.get());
                assertEquals(0, subscriber.monitor.closeCalls.get());
                assertEquals(0, subscriber.closeCalls.get());
            } finally {
                subscriber.releaseReceive.countDown();
            }
            stop.get(2, TimeUnit.SECONDS);
            assertEquals(1, subscriber.disconnectCalls.get());
            assertEquals(1, subscriber.monitor.closeCalls.get());
            assertEquals(1, subscriber.closeCalls.get());
            assertEquals(List.of(
                "subscribe-enter", "subscribe-exit", "disconnect", "close"),
                subscriber.events);
        }
    }

    @Test
    void connectionSnapshotIsNotReceivableBeforeConnectCommit()
        throws Exception {
        TestStore store = new TestStore();
        store.rows = List.of(descriptor());
        try (Fixture fixture = new Fixture(store, true)) {
            fixture.start();
            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            assertTrue(subscriber.connectEntered.await(1, TimeUnit.SECONDS));

            try {
                assertTrue(fixture.runtime.publisherSnapshots("events").isEmpty());
                assertEquals(0, subscriber.readinessWaits);
                assertEquals(0, subscriber.subscribeCalls);
            } finally {
                subscriber.releaseConnect.countDown();
            }

            awaitCondition(() -> !fixture.runtime
                .publisherSnapshots("events").isEmpty());
            subscriber.readinessObserved.get(1, TimeUnit.SECONDS);
            assertTrue(subscriber.readinessWaits > 0);
        }
    }

    @Test
    void lateMonitorEventCannotRemoveSuccessorConnection() throws Exception {
        ZLinkFanoutPublisherDescriptor descriptor = descriptor();
        TestStore store = new TestStore();
        store.rows = List.of(descriptor);
        try (Fixture fixture = new Fixture(store)) {
            fixture.start();

            ControlledSubscriber first = fixture.awaitSubscriber();
            first.monitor.emit("DISCONNECTED");
            ControlledSubscriber successor = fixture.awaitSubscriber();
            assertTrue(first.closed);
            first.monitor.emit("DISCONNECTED");

            assertFalse(successor.closed);
            assertEquals(2, fixture.subscribers.size());
        }
    }

    @Test
    void completedReconcileCannotOpenConnectionAfterStop() throws Exception {
        TestStore store = new TestStore();
        store.blockNextRead();
        try (Fixture fixture = new Fixture(store)) {
            fixture.start();
            store.readStarted.get(1, TimeUnit.SECONDS);
            CompletionStage<Void> stopped = fixture.runtime.stop();
            assertFalse(stopped.toCompletableFuture().isDone());

            store.completeBlockedRead(List.of(descriptor()));
            stopped.toCompletableFuture().get(1, TimeUnit.SECONDS);

            assertNull(fixture.created.poll(100, TimeUnit.MILLISECONDS));
        }
    }

    @Test
    void subscriberReceiveRequiresSocketReadiness() throws Exception {
        TestStore store = new TestStore();
        store.rows = List.of(descriptor());
        try (Fixture fixture = new Fixture(store)) {
            fixture.start();

            ControlledSubscriber subscriber = fixture.awaitSubscriber();
            subscriber.readinessObserved.get(1, TimeUnit.SECONDS);
            assertTrue(subscriber.readinessWaits > 0);
            assertEquals(0, subscriber.subscribeCalls);
        }
    }

    @Test
    void blockingProviderAndSaturatedTicksDoNotDelayRequestTimeout()
        throws Exception {
        SaturatingStore store = new SaturatingStore();
        try (Fixture fixture = new Fixture(store)) {
            fixture.start();
            assertTrue(store.entered.await(1, TimeUnit.SECONDS));

            ZLinkChannelCallRuntime calls = new ZLinkChannelCallRuntime(
                null,
                fixture.scheduler,
                new ZLinkChannelReplyDecoder(
                    new ZLinkJsonMessageSerializer()),
                (channel, node, spot, generation,
                 authorityOwnerGeneration, ownerLeaseGeneration, parts) ->
                    CompletableFuture.completedFuture(null),
                (channel, node, spot, generation,
                 authorityOwnerGeneration, ownerLeaseGeneration, parts,
                 timeout) ->
                    CompletableFuture.completedFuture(List.of()));
            CompletableFuture<Void> request = new CompletableFuture<>();
            try {
                long startedNanos = System.nanoTime();
                calls.track(request, Duration.ofMillis(40));

                ExecutionException failure = assertThrows(
                    ExecutionException.class,
                    () -> request.get(500, TimeUnit.MILLISECONDS));
                ZLinkFrameworkException timeout = assertInstanceOf(
                    ZLinkFrameworkException.class, failure.getCause());
                assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, timeout.kind());
                assertInstanceOf(
                    TimeoutException.class, timeout.getCause());
                assertTrue(
                    TimeUnit.NANOSECONDS.toMillis(
                        System.nanoTime() - startedNanos) < 250,
                    "the request deadline must not wait for provider progress");

                Thread.sleep(60);
                assertEquals(1, store.listCalls.get());
            } finally {
                calls.beginClose();
                store.release.countDown();
            }
        }
    }

    private static ZLinkFanoutPublisherDescriptor descriptor() {
        return new ZLinkFanoutPublisherDescriptor(
            "events",
            RoutingId.from("publisher"),
            7,
            1,
            "tcp://127.0.0.1:7001",
            ZLinkFrameworkRuntimeState.SERVING,
            "default",
            "owner",
            3,
            Instant.now());
    }

    private static void awaitCondition(
        java.util.function.BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("condition timed out");
            }
            Thread.sleep(1);
        }
    }

    private static final class Fixture implements AutoCloseable {
        private final List<ControlledSubscriber> subscribers =
            new CopyOnWriteArrayList<>();
        private final LinkedBlockingQueue<ControlledSubscriber> created =
            new LinkedBlockingQueue<>();
        private final ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        private final ExecutorService infrastructure =
            Executors.newVirtualThreadPerTaskExecutor();
        private final ZLinkFanoutLocationRuntime runtime;

        private Fixture(ZLinkLocationRepository store) {
            this(store, false);
        }

        private Fixture(
            ZLinkLocationRepository store,
            boolean blockConnect) {
            runtime = new ZLinkFanoutLocationRuntime(
                store,
                () -> new ZLinkLocationOwnerToken("owner", 3),
                new Backend(subscribers, created, blockConnect),
                socket -> ((ControlledSubscriber) socket).monitor,
                new Context(),
                new ZLinkChannelSocketRegistry(),
                scheduler,
                infrastructure,
                Duration.ofMillis(1),
                100,
                (channel, message) ->
                    message.parts().forEach(
                        Message::close));
        }

        private void start() {
            runtime.start(List.of(new ZLinkChannelRuntime.AutoConnectSurface(
                systems.zlink.framework.runtime.internal.locations
                    .ZLinkAutoConnectType.FANOUT,
                "events",
                systems.zlink.framework.locations.ZLinkLocationRole.SUB,
                RoutingId.from("subscriber"),
                "",
                100,
                null,
                List.of()))).toCompletableFuture().join();
        }

        private ControlledSubscriber awaitSubscriber() throws Exception {
            ControlledSubscriber value = created.poll(1, TimeUnit.SECONDS);
            if (value == null) {
                throw new AssertionError("fanout subscriber was not created");
            }
            value.monitor.handlerReady.get(1, TimeUnit.SECONDS);
            return value;
        }

        @Override
        public void close() {
            subscribers.forEach(subscriber -> {
                subscriber.releaseConnect.countDown();
                subscriber.releaseReceive.countDown();
            });
            runtime.close();
            scheduler.shutdownNow();
            infrastructure.shutdownNow();
        }
    }

    private static final class SaturatingStore
        extends ZLinkLocationStoreTestAdapter {
        private final CountDownLatch entered = new CountDownLatch(1);
        private final CountDownLatch release = new CountDownLatch(1);
        private final AtomicInteger listCalls = new AtomicInteger();

        @Override
        public CompletionStage<ZLinkLocationWriteResult>
            updateFanoutPublisher(
                ZLinkFanoutPublisherDescriptor descriptor,
                ZLinkLocationWriteIntent intent) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteResult.stored(1, Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkLocationWriteStatus>
            removeFanoutPublisher(
                ZLinkFanoutPublisherDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.STORED);
        }

        @Override
        public CompletionStage<
            ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
            listFanoutPublishers(
                String channelName,
                ZLinkPageRequest page) {
            listCalls.incrementAndGet();
            entered.countDown();
            try {
                if (!release.await(1, TimeUnit.SECONDS)) {
                    throw new AssertionError(
                        "provider test release was not signalled");
                }
            } catch (InterruptedException interrupted) {
                Thread.currentThread().interrupt();
                throw new AssertionError(interrupted);
            }
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(List.of(), null));
        }
    }

    private static final class TestStore
        extends ZLinkLocationStoreTestAdapter {
        private volatile List<ZLinkFanoutPublisherDescriptor> rows = List.of();
        private volatile CompletableFuture<
            ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> blockedRead;
        private final CompletableFuture<Void> readStarted =
            new CompletableFuture<>();

        private void blockNextRead() {
            blockedRead = new CompletableFuture<>();
        }

        private void completeBlockedRead(
            List<ZLinkFanoutPublisherDescriptor> values) {
            blockedRead.complete(new ZLinkLocationPage<>(values, null));
        }

        @Override
        public CompletionStage<ZLinkLocationWriteResult>
            updateFanoutPublisher(
                ZLinkFanoutPublisherDescriptor descriptor,
                ZLinkLocationWriteIntent
                    intent) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteResult.stored(1, Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkLocationWriteStatus>
            removeFanoutPublisher(
                ZLinkFanoutPublisherDescriptorKey key,
                ZLinkLocationOwnerToken owner) {
            return CompletableFuture.completedFuture(
                ZLinkLocationWriteStatus.STORED);
        }

        @Override
        public CompletionStage<
            ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
            listFanoutPublishers(
                String channelName,
                ZLinkPageRequest page) {
            CompletableFuture<
                ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> blocked =
                blockedRead;
            if (blocked != null) {
                readStarted.complete(null);
                return blocked;
            }
            return CompletableFuture.completedFuture(
                new ZLinkLocationPage<>(rows, null));
        }
    }

    private static final class Backend implements ZLinkChannelBackendAdapter {
        private final List<ControlledSubscriber> subscribers;
        private final LinkedBlockingQueue<ControlledSubscriber> created;
        private final boolean blockConnect;

        private Backend(
            List<ControlledSubscriber> subscribers,
            LinkedBlockingQueue<ControlledSubscriber> created,
            boolean blockConnect) {
            this.subscribers = subscribers;
            this.created = created;
            this.blockConnect = blockConnect;
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(
            ZLinkBackendContext context) {
            ControlledSubscriber subscriber = new ControlledSubscriber(
                blockConnect);
            subscribers.add(subscriber);
            created.add(subscriber);
            return subscriber;
        }

        @Override
        public ZLinkBackendContext createContext() {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(
            ZLinkBackendContext context) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class ControlledSubscriber
        implements ZLinkBackendSubscriberSocket {
        private final Monitor monitor = new Monitor();
        private volatile boolean closed;
        private volatile int readinessWaits;
        private volatile int subscribeCalls;
        private final AtomicBoolean readable = new AtomicBoolean();
        private final AtomicBoolean blockReceive = new AtomicBoolean();
        private final boolean blockConnect;
        private final CountDownLatch connectEntered = new CountDownLatch(1);
        private final CountDownLatch releaseConnect = new CountDownLatch(1);
        private final CountDownLatch subscribeEntered = new CountDownLatch(1);
        private final CountDownLatch releaseReceive = new CountDownLatch(1);
        private final AtomicInteger disconnectCalls = new AtomicInteger();
        private final AtomicInteger closeCalls = new AtomicInteger();
        private final List<String> events = new CopyOnWriteArrayList<>();
        private final CompletableFuture<Void> readinessObserved =
            new CompletableFuture<>();

        private ControlledSubscriber(boolean blockConnect) {
            this.blockConnect = blockConnect;
        }

        private void blockReceive() {
            blockReceive.set(true);
            readable.set(true);
        }

        @Override
        public void setChannelName(String channelName) {
        }

        @Override
        public void setSubscription(String topic) {
        }

        @Override
        public ZLinkBackendTopicMessage subscribe(
            ZLinkBackendRecvMode mode) {
            subscribeCalls++;
            if (blockReceive.get()) {
                events.add("subscribe-enter");
                subscribeEntered.countDown();
                boolean interrupted = false;
                while (true) {
                    try {
                        releaseReceive.await();
                        break;
                    } catch (InterruptedException ignored) {
                        interrupted = true;
                    }
                }
                if (interrupted) {
                    Thread.currentThread().interrupt();
                }
                events.add("subscribe-exit");
                return null;
            }
            throw new AssertionError("subscriber recv was called without readiness");
        }

        @Override
        public boolean waitForReadable(Duration timeout) {
            readinessWaits++;
            readinessObserved.complete(null);
            return readable.get();
        }

        @Override
        public void connect(String endpoint) {
            connectEntered.countDown();
            if (blockConnect) {
                boolean interrupted = false;
                while (true) {
                    try {
                        releaseConnect.await();
                        break;
                    } catch (InterruptedException ignored) {
                        interrupted = true;
                    }
                }
                if (interrupted) {
                    Thread.currentThread().interrupt();
                }
            }
        }

        @Override
        public void disconnect(String endpoint) {
            disconnectCalls.incrementAndGet();
            events.add("disconnect");
        }

        @Override
        public void bind(String endpoint) {
            throw new UnsupportedOperationException();
        }

        @Override
        public String name() {
            return "subscriber";
        }

        @Override
        public void close() {
            closed = true;
            closeCalls.incrementAndGet();
            events.add("close");
        }
    }

    private static final class Monitor implements ZLinkBackendSocketMonitor {
        private ZLinkBackendSocketMonitorHandler handler;
        private final AtomicInteger closeCalls = new AtomicInteger();
        private final CompletableFuture<Void> handlerReady =
            new CompletableFuture<>();

        private void emit(String event) {
            handler.handle(new ZLinkBackendSocketMonitorEvent(
                event, Optional.empty(), "", ""));
        }

        @Override
        public void onEvent(ZLinkBackendSocketMonitorHandler value) {
            handler = value;
            handlerReady.complete(null);
        }

        @Override
        public ZLinkBackendSocketMonitorEvent recv() {
            return null;
        }

        @Override
        public String name() {
            return "monitor";
        }

        @Override
        public void close() {
            closeCalls.incrementAndGet();
        }
    }

    private static final class Context implements ZLinkBackendContext {
        @Override
        public void shutdown() {
        }

        @Override
        public String name() {
            return "context";
        }

        @Override
        public void close() {
        }
    }
}
