package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Channel teardown must observe binding completion before destroying resources. */
final class ZLinkChannelShutdownInFlightTest {
    private static final long WAIT_SECONDS = 30;

    @Test
    void closeObservesInFlightBeaconPublishBeforeClosingNativeResources() throws Exception {
        assertCloseObservesBinding(false);
    }

    @Test
    void closeObservesInFlightClientServerDealerLivenessBeforeClosingNativeResources()
            throws Exception {
        assertCloseObservesBinding(true);
    }

    private void assertCloseObservesBinding(boolean dealerLiveness) throws Exception {
        List<String> events = new CopyOnWriteArrayList<>();
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        BlockingChannelBackend backend = new BlockingChannelBackend(events, entered, release);
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        if (!dealerLiveness)
            options.addFanoutChannel("fanout").enablePublisher("inproc://c1876-beacon");

        ZLinkChannelRuntime runtime =
                new ZLinkChannelRuntime(
                        backend,
                        options.registration(),
                        new ZLinkJsonMessageSerializer(),
                        ZLinkHandlerActivator.services());
        var executorField = ZLinkChannelRuntime.class.getDeclaredField("infrastructureExecutor");
        executorField.setAccessible(true);
        ExecutorService executor = (ExecutorService) executorField.get(runtime);
        CompletableFuture<Void> waiting = new CompletableFuture<>();
        executorField.set(runtime, observeExecutor(executor, waiting, false));
        if (dealerLiveness) {
            var socketsField = ZLinkChannelRuntime.class.getDeclaredField("sockets");
            socketsField.setAccessible(true);
            var sockets = (ZLinkChannelSocketRegistry) socketsField.get(runtime);
            var descriptor =
                    new ZLinkClientServerServerDescriptor(
                            "orders",
                            RoutingId.from("server"),
                            1,
                            1,
                            "inproc://c1876-dealer",
                            100,
                            ZLinkFrameworkRuntimeState.SERVING,
                            "default",
                            "owner",
                            1,
                            Instant.EPOCH);
            var dealer =
                    (ZLinkBackendDealerSocket)
                            Proxy.newProxyInstance(
                                    ZLinkBackendDealerSocket.class.getClassLoader(),
                                    new Class<?>[] {ZLinkBackendDealerSocket.class},
                                    (proxy, method, args) -> {
                                        return switch (method.getName()) {
                                            case "send" -> {
                                                @SuppressWarnings("unchecked")
                                                var parts = (List<Message>) args[0];
                                                assertInstanceOf(
                                                        ZLinkClientServerServiceWire.LivenessProbe
                                                                .class,
                                                        ZLinkClientServerServiceWire.decode(
                                                                parts.getFirst().toByteArray()));
                                                backend.publisher.publish(
                                                        "liveness", List.of(), SendFlags.DONT_WAIT);
                                                yield CompletableFuture.completedFuture(null);
                                            }
                                            case "close" -> {
                                                backend.publisher.close();
                                                yield null;
                                            }
                                            case "name" -> "dealer";
                                            case "equals" -> proxy == args[0];
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            default -> {
                                                if (method.isDefault())
                                                    yield InvocationHandler.invokeDefault(
                                                            proxy, method, args);
                                                yield null;
                                            }
                                        };
                                    });
            sockets.addClientServerConnection("liveness", descriptor, dealer);
            assertTrue(
                    sockets.admitClientServerConnection(
                            "liveness",
                            descriptor,
                            sockets.clientServerTransportReady("liveness")));
            installLocationOwner(runtime, backend, sockets, executor, descriptor, dealer);
            // Advance the liveness input, without a timer or wall-clock wait, on the same
            // infrastructure executor used by the production periodic source.
            executor.execute(
                    () ->
                            sockets.tickClientServerLiveness(
                                    System.nanoTime()
                                            + ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL
                                                    .toNanos()));
        }
        CompletableFuture<Void> closing = null;
        try {
            assertTrue(entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            closing =
                    runtime.closeAsync(Instant.now().plusSeconds(WAIT_SECONDS))
                            .toCompletableFuture();
            CompletableFuture.anyOf(waiting, closing).get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(closing.isDone(), "close completed before the binding returned");
            assertFalse(backend.publisher.closed.get());
            assertFalse(backend.context.closed.get());
        } finally {
            release.countDown();
            if (closing != null) closing.get(WAIT_SECONDS, TimeUnit.SECONDS);
            else runtime.close();
            assertTrue(executor.awaitTermination(WAIT_SECONDS, TimeUnit.SECONDS));
        }
        assertTrue(backend.publisher.closed.get());
        assertTrue(backend.context.closed.get());
        events.forEach(event -> System.out.println("[c1876-channel] " + event));

        assertEquals(
                0,
                backend.publisher.closedWhileInFlight.get(),
                "publisher socket was closed while its beacon publish was in flight");
        assertEquals(
                0,
                backend.context.closedWhileInFlight.get(),
                "owned context was closed while a beacon publish was in flight");
        assertEquals(
                0,
                backend.publisher.useAfterClose.get(),
                "released beacon publish touched an already closed publisher/context");
    }

    private static void installLocationOwner(
            ZLinkChannelRuntime runtime,
            BlockingChannelBackend backend,
            ZLinkChannelSocketRegistry sockets,
            ExecutorService executor,
            ZLinkClientServerServerDescriptor descriptor,
            ZLinkBackendDealerSocket dealer)
            throws Exception {
        var schedulerField = ZLinkChannelRuntime.class.getDeclaredField("timeoutExecutor");
        schedulerField.setAccessible(true);
        var repository =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkLocationRepository.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, args) -> {
                                    throw new AssertionError(
                                            "unexpected store call: " + method.getName());
                                });
        var provider =
                (ZLinkBackendAdapterProvider)
                        Proxy.newProxyInstance(
                                ZLinkBackendAdapterProvider.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendAdapterProvider.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("createChannelAdapter"))
                                        return backend;
                                    throw new AssertionError(
                                            "unexpected adapter: " + method.getName());
                                });
        var location =
                new ZLinkClientServerLocationRuntime(
                        repository,
                        () -> null,
                        provider,
                        backend.context,
                        new ZLinkBackendAdapterOptions(Duration.ofSeconds(WAIT_SECONDS)),
                        sockets,
                        (ScheduledExecutorService) schedulerField.get(runtime),
                        executor,
                        Duration.ofSeconds(WAIT_SECONDS),
                        1);
        var runningField = ZLinkClientServerLocationRuntime.class.getDeclaredField("running");
        runningField.setAccessible(true);
        runningField.setBoolean(location, true);
        var connectionClass =
                Class.forName(ZLinkClientServerLocationRuntime.class.getName() + "$Connection");
        var constructor = connectionClass.getDeclaredConstructors()[0];
        constructor.setAccessible(true);
        var connectionsField =
                ZLinkClientServerLocationRuntime.class.getDeclaredField("connections");
        connectionsField.setAccessible(true);
        @SuppressWarnings("unchecked")
        var connections = (Map<String, Object>) connectionsField.get(location);
        connections.put("liveness", constructor.newInstance("liveness", descriptor, dealer));
        var locationField =
                ZLinkChannelRuntime.class.getDeclaredField("clientServerLocationRuntime");
        locationField.setAccessible(true);
        locationField.set(runtime, location);
    }

    @Test
    void expiredExecutorSettlementDoesNotCloseInFlightResources() throws Exception {
        var events = new CopyOnWriteArrayList<String>();
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var backend = new BlockingChannelBackend(events, entered, release);
        var options = new DefaultZLinkFrameworkOptions();
        options.addFanoutChannel("fanout").enablePublisher("inproc://c1876-expired");
        var runtime =
                new ZLinkChannelRuntime(
                        backend,
                        options.registration(),
                        new ZLinkJsonMessageSerializer(),
                        ZLinkHandlerActivator.services());
        var executorField = ZLinkChannelRuntime.class.getDeclaredField("infrastructureExecutor");
        executorField.setAccessible(true);
        ExecutorService executor = (ExecutorService) executorField.get(runtime);
        var waiting = new CompletableFuture<Void>();
        executorField.set(runtime, observeExecutor(executor, waiting, true));
        CompletableFuture<Void> closing = null;
        try {
            assertTrue(entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            closing =
                    runtime.closeAsync(Instant.now().plusSeconds(WAIT_SECONDS))
                            .toCompletableFuture();
            CompletableFuture.anyOf(waiting, closing).get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(
                    closing.isDone(), "expired deadline must still observe binding termination");
            assertFalse(backend.publisher.closed.get());
            assertFalse(backend.context.closed.get());
        } finally {
            release.countDown();
            if (closing != null)
                closing.handle((ignored, failure) -> null).get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertTrue(executor.awaitTermination(WAIT_SECONDS, TimeUnit.SECONDS));
            executorField.set(runtime, executor);
            runtime.close();
        }
        var result = closing;
        var failure =
                assertThrows(
                        ExecutionException.class, () -> result.get(WAIT_SECONDS, TimeUnit.SECONDS));
        assertInstanceOf(TimeoutException.class, failure.getCause());
        assertTrue(backend.publisher.closed.get());
        assertTrue(backend.context.closed.get());
        assertEquals(0, backend.publisher.useAfterClose.get());
    }

    private static ExecutorService observeExecutor(
            ExecutorService executor, CompletableFuture<Void> waiting, boolean expired) {
        var waits = new AtomicInteger();
        return (ExecutorService)
                Proxy.newProxyInstance(
                        ExecutorService.class.getClassLoader(),
                        new Class<?>[] {ExecutorService.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("awaitTermination")) {
                                if (expired && waits.getAndIncrement() == 0) return false;
                                waiting.complete(null);
                            }
                            try {
                                return method.invoke(executor, arguments);
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static final class BlockingChannelBackend implements ZLinkChannelBackendAdapter {
        final RecordingContext context;
        final BlockingPublisher publisher;

        BlockingChannelBackend(
                List<String> events, CountDownLatch entered, CountDownLatch release) {
            this.publisher = new BlockingPublisher(events, entered, release);
            this.context = new RecordingContext(events, publisher);
            this.publisher.context = context;
        }

        @Override
        public ZLinkBackendContext createContext() {
            return context;
        }

        @Override
        public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException("dealer is not part of this fixture");
        }

        @Override
        public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException("router is not part of this fixture");
        }

        @Override
        public ZLinkBackendPublisherSocket createPublisherSocket(
                ZLinkBackendContext context, Duration sendTimeout) {
            return publisher;
        }

        @Override
        public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) {
            throw new UnsupportedOperationException("subscriber is not part of this fixture");
        }
    }

    private static final class RecordingContext implements ZLinkBackendContext {
        private final List<String> events;
        private final BlockingPublisher publisher;
        final AtomicBoolean closed = new AtomicBoolean();
        final AtomicInteger closedWhileInFlight = new AtomicInteger();

        RecordingContext(List<String> events, BlockingPublisher publisher) {
            this.events = events;
            this.publisher = publisher;
        }

        @Override
        public String name() {
            return "context";
        }

        @Override
        public void shutdown() {
            events.add("native: context.shutdown, publish in flight=" + publisher.inFlight.get());
        }

        @Override
        public void close() {
            boolean inFlight = publisher.inFlight.get();
            if (inFlight) closedWhileInFlight.incrementAndGet();
            closed.set(true);
            events.add("native: context.close, publish in flight=" + inFlight);
        }
    }

    private static final class BlockingPublisher implements ZLinkBackendPublisherSocket {
        private final List<String> events;
        private final CountDownLatch entered;
        private final CountDownLatch release;
        private final AtomicBoolean first = new AtomicBoolean(true);
        RecordingContext context;
        final AtomicBoolean inFlight = new AtomicBoolean();
        final AtomicBoolean closed = new AtomicBoolean();
        final AtomicInteger closedWhileInFlight = new AtomicInteger();
        final AtomicInteger useAfterClose = new AtomicInteger();

        BlockingPublisher(List<String> events, CountDownLatch entered, CountDownLatch release) {
            this.events = events;
            this.entered = entered;
            this.release = release;
        }

        @Override
        public String name() {
            return "publisher";
        }

        @Override
        public void bind(String endpoint) {}

        @Override
        public void setChannelName(String channelName) {}

        @Override
        public void setRoutingId(RoutingId routingId) {}

        @Override
        public void setNoDrop(boolean noDrop) {}

        @Override
        public boolean publish(String topic, List<Message> parts, SendFlags flags) {
            if (!first.compareAndSet(true, false)) {
                return true;
            }
            // Models the window after the binding's open check and before the native send
            // returns: the handle has been read and the call is inside the native library.
            inFlight.set(true);
            events.add("binding: beacon publish entered on " + Thread.currentThread().getName());
            entered.countDown();
            boolean interrupted = false;
            while (true) {
                try {
                    release.await();
                    break;
                } catch (InterruptedException ignored) {
                    interrupted = true;
                }
            }
            boolean socketClosed = closed.get();
            boolean contextClosed = context.closed.get();
            if (socketClosed || contextClosed) useAfterClose.incrementAndGet();
            events.add(
                    "binding: beacon publish resumed, publisher closed="
                            + socketClosed
                            + ", context closed="
                            + contextClosed
                            + ", interrupted="
                            + interrupted);
            inFlight.set(false);
            if (interrupted) Thread.currentThread().interrupt();
            return true;
        }

        @Override
        public void close() {
            boolean busy = inFlight.get();
            if (busy) closedWhileInFlight.incrementAndGet();
            closed.set(true);
            events.add("native: publisher.close, publish in flight=" + busy);
        }
    }
}
