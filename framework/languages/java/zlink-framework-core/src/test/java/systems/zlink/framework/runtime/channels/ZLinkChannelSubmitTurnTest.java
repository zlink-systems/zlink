package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.Optional;
import java.util.Map;
import java.util.Queue;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;

final class ZLinkChannelSubmitTurnTest {
    private static final Duration DEFAULT_TIMEOUT = Duration.ofSeconds(30);
    private static final IllegalStateException TERMINAL = new IllegalStateException("test terminal");

    @Test
    void buildersUseTheRegistrationNodeAndDefaultTimeoutAtSubmit() throws Exception {
        try (Fixture f = new Fixture()) {
            AtomicInteger oldCalls = new AtomicInteger();
            AtomicInteger newCalls = new AtomicInteger();
            AtomicReference<Duration> observedTimeout = new AtomicReference<>();
            f.sockets.registerSpotRouterNode("orders", f.node(oldCalls, observedTimeout, () -> { }));
            f.register(Duration.ofSeconds(2));
            ZLinkRequestCall request = f.request();
            ZLinkSendCall send = f.send();
            f.register(Duration.ofSeconds(7));
            f.sockets.registerSpotRouterNode("orders", f.node(newCalls, observedTimeout, () -> { }));

            assertTerminal(request.submit(String.class));
            send.submit().toCompletableFuture().join();
            assertEquals(0, oldCalls.get(), "builders must not capture the old node");
            assertEquals(2, newCalls.get());
            assertEquals(Duration.ofSeconds(7), observedTimeout.get());
            assertTerminal(f.request().timeout(Duration.ofSeconds(3)).submit(String.class));
            assertEquals(Duration.ofSeconds(3), observedTimeout.get());
        }
    }

    @Test
    void buildersCanPrecedeRegistrationAndShareTheSingleUseGate() throws Exception {
        try (Fixture f = new Fixture()) {
            ZLinkRequestCall request = f.request();
            ZLinkSendCall send = f.send();
            AtomicInteger calls = new AtomicInteger();
            f.sockets.registerSpotRouterNode("orders", f.node(calls, new AtomicReference<>(), () -> { }));
            assertTerminal(request.timeout(Duration.ofSeconds(1)).submit(String.class));
            send.metadata("key", "value").submit().toCompletableFuture().join();
            assertThrows(CompletionException.class, () -> request.submit(String.class).toCompletableFuture().join());
            assertThrows(CompletionException.class, () -> send.submit().toCompletableFuture().join());
            assertEquals(2, calls.get());
        }
    }

    @Test
    void clientServerStillRejectsExplicitMetadataIncludingAnEmptyMapBeforeReadinessWait() throws Exception {
        try (Fixture f = new Fixture()) {
            ChannelRegistration channel = new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
            channel.enableClient();
            f.sockets.registerChannel(channel);
            for (Map<String, String> values : java.util.List.of(Map.<String, String>of(), Map.of("key", "value"))) {
                ZLinkSendCall send = f.send().metadata(values);
                ZLinkRequestCall request = f.request().metadata(values);
                assertThrows(UnsupportedOperationException.class, send::submit);
                assertInstanceOf(UnsupportedOperationException.class,
                    assertThrows(CompletionException.class,
                        () -> request.submit(String.class).toCompletableFuture().join()).getCause());
            }
        }
    }

    @Test
    void requestAndSendStartBindingInTheRegistryTurnIncludingAnExistingTurn() throws Exception {
        for (boolean alreadyOnLane : new boolean[] {false, true}) {
            try (Fixture f = new Fixture()) {
                AtomicInteger calls = new AtomicInteger();
                f.sockets.registerSpotRouterNode("orders", f.node(calls, new AtomicReference<>(),
                    () -> assertSame(f.lane, ZLinkStateLane.current())));
                ZLinkRequestCall request = f.request();
                ZLinkSendCall send = f.send();
                CompletionStage<String> reply = alreadyOnLane
                    ? f.lane.runAsync(() -> request.submit(String.class)).toCompletableFuture().join()
                    : request.submit(String.class);
                CompletionStage<Void> admission = alreadyOnLane
                    ? f.lane.runAsync(send::submit).toCompletableFuture().join() : send.submit();
                assertTerminal(reply);
                admission.toCompletableFuture().join();
                assertEquals(2, calls.get());
            }
        }
    }

    @Test
    void nodeReplacementCannotInterleaveBetweenSelectionAndBindingSubmit() throws Exception {
        for (boolean request : new boolean[] {false, true}) {
            try (Fixture f = new Fixture(); var workers = Executors.newVirtualThreadPerTaskExecutor()) {
                CountDownLatch bindingEntered = new CountDownLatch(1);
                CountDownLatch releaseBinding = new CountDownLatch(1);
                AtomicInteger oldCalls = new AtomicInteger();
                AtomicInteger newCalls = new AtomicInteger();
                f.sockets.registerSpotRouterNode("orders", f.node(oldCalls, new AtomicReference<>(), () -> {
                    assertSame(f.lane, ZLinkStateLane.current());
                    bindingEntered.countDown();
                    await(releaseBinding);
                }));
                var submission = workers.submit(() -> request ? f.request().submit(String.class) : f.send().submit());
                try {
                    await(bindingEntered);
                    var replacement = workers.submit(() -> f.sockets.registerSpotRouterNode("orders",
                        f.node(newCalls, new AtomicReference<>(), () -> { })));
                    awaitQueued(f.lane);
                    assertFalse(replacement.isDone());
                    releaseBinding.countDown();
                    CompletionStage<?> completion = submission.get(5, TimeUnit.SECONDS);
                    if (request) assertTerminal(completion);
                    else completion.toCompletableFuture().join();
                    replacement.get(5, TimeUnit.SECONDS);
                    assertTerminal(f.request().submit(String.class));
                    assertEquals(1, oldCalls.get());
                    assertEquals(1, newCalls.get());
                } finally {
                    releaseBinding.countDown();
                }
            }
        }
    }

    @Test
    void clientRemovalCannotCloseTheSelectedDealerBeforeSubmissionReturns() throws Exception {
        try (Fixture f = new Fixture(); var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            ChannelRegistration channel = new ChannelRegistration("orders", ChannelKind.CLIENT_SERVER);
            channel.enableClient();
            f.sockets.registerChannel(channel);
            CountDownLatch bindingEntered = new CountDownLatch(1);
            CountDownLatch releaseBinding = new CountDownLatch(1);
            AtomicInteger closes = new AtomicInteger();
            ZLinkBackendDealerSocket dealer = (ZLinkBackendDealerSocket) Proxy.newProxyInstance(
                getClass().getClassLoader(), new Class<?>[] {ZLinkBackendDealerSocket.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("close")) closes.incrementAndGet();
                    return null;
                });
            var descriptor = new ZLinkClientServerServerDescriptor("orders", RoutingId.from("server"),
                1, 1, "tcp://127.0.0.1:7001", 100, ZLinkFrameworkRuntimeState.SERVING,
                "test", "owner", 1, Instant.EPOCH);
            f.sockets.addClientServerConnection("connection", descriptor, dealer);
            assertTrue(f.sockets.admitClientServerConnection("connection", descriptor));
            var submission = workers.submit(() -> f.sockets.submitToChannel("orders", null, DEFAULT_TIMEOUT, false,
                (target, remaining) -> {
                    assertSame(dealer, target);
                    assertSame(f.lane, ZLinkStateLane.current());
                    bindingEntered.countDown();
                    await(releaseBinding);
                    assertEquals(0, closes.get());
                    return CompletableFuture.completedFuture(null);
                }, (node, timeout) -> { throw new AssertionError("expected ClientServer"); }));
            try {
                await(bindingEntered);
                var removal = workers.submit(() -> f.sockets.removeClientServerConnection("connection"));
                awaitQueued(f.lane);
                assertFalse(removal.isDone());
                assertEquals(0, closes.get());
                releaseBinding.countDown();
                submission.get(5, TimeUnit.SECONDS).toCompletableFuture().join();
                removal.get(5, TimeUnit.SECONDS);
                assertEquals(1, closes.get());
                assertNull(f.sockets.clientForOutbound("orders"));
            } finally {
                releaseBinding.countDown();
            }
        }
    }

    private static void assertTerminal(CompletionStage<?> stage) {
        assertSame(TERMINAL, assertThrows(CompletionException.class,
            () -> stage.toCompletableFuture().join()).getCause());
    }

    private static void await(CountDownLatch latch) {
        try {
            assertTrue(latch.await(5, TimeUnit.SECONDS));
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }

    private static void awaitQueued(ZLinkStateLane lane) throws Exception {
        Queue<?> mailbox = (Queue<?>) field(lane, "mailbox");
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (mailbox.isEmpty() && System.nanoTime() < deadline) Thread.yield();
        assertFalse(mailbox.isEmpty(), "the mutation must be queued behind the submission turn");
    }

    private static Object field(Object object, String name) throws Exception {
        Field field = object.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(object);
    }

    private static final class Fixture implements AutoCloseable {
        final ZLinkChannelSocketRegistry sockets = new ZLinkChannelSocketRegistry();
        final ZLinkStateLane lane = (ZLinkStateLane) field(sockets, "stateLane");
        final java.util.concurrent.ScheduledExecutorService scheduler = Executors.newSingleThreadScheduledExecutor();
        final ZLinkChannelCallRuntime runtime;

        Fixture() throws Exception {
            var options = new DefaultZLinkFrameworkOptions();
            runtime = new ZLinkChannelCallRuntime(new ZLinkMessageFlowTracer(
                options.registration().dispatchOptions(), null, Runnable::run), scheduler, null, null, null);
        }

        void register(Duration timeout) {
            ChannelRegistration registration = new ChannelRegistration("orders", ChannelKind.ROUTE_MESH);
            registration.setDefaultRequestTimeout(timeout);
            sockets.registerChannel(registration);
        }

        ZLinkRequestCall request() {
            return new ChannelRequestCall(runtime, "orders", sockets, DEFAULT_TIMEOUT,
                Message.from("request"), Optional.of("packet"), null);
        }

        ZLinkSendCall send() {
            return new ChannelSendCall(runtime, "orders", sockets, DEFAULT_TIMEOUT,
                Message.from("send"), Optional.of("packet"));
        }

        ZLinkInternalSpotNode node(AtomicInteger calls, AtomicReference<Duration> timeout, Runnable binding) {
            return (ZLinkInternalSpotNode) Proxy.newProxyInstance(getClass().getClassLoader(),
                new Class<?>[] {ZLinkInternalSpotNode.class}, (proxy, method, args) -> {
                    if (method.getName().equals("name")) {
                        return "orders";
                    }
                    if (method.getName().equals("requestToChannel")) {
                        timeout.set((Duration) args[3]);
                        var operations = (ZLinkServiceOperationRegistry) args[4];
                        return operations.submit((UUID) args[5], (Duration) args[3], () -> {
                            assertEquals(1, operations.pendingCount(), "registration precedes binding submission");
                            calls.incrementAndGet();
                            binding.run();
                            return CompletableFuture.failedFuture(TERMINAL);
                        }, ignored -> { });
                    }
                    if (method.getName().equals("sendToChannel")) {
                        calls.incrementAndGet();
                        binding.run();
                        return CompletableFuture.completedFuture(null);
                    }
                    throw new AssertionError("unexpected backend call: " + method);
                });
        }

        @Override
        public void close() {
            runtime.beginClose();
            scheduler.shutdownNow();
            lane.closeAsync().toCompletableFuture().join();
        }
    }
}
