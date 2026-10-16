package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.configuration.ZLinkLegacyTopology;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

/** Spot teardown must settle publisher workers before closing binding resources. */
final class ZLinkSpotShutdownInFlightTest {
    private static final long WAIT_SECONDS = 30;

    @Test
    void closeObservesInFlightPoolPublishBeforeClosingNode() throws Exception {
        List<String> events = new CopyOnWriteArrayList<>();
        CountDownLatch entered = new CountDownLatch(1);
        CountDownLatch release = new CountDownLatch(1);
        Probe probe = new Probe(events, entered, release);

        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        ZLinkLegacyTopology.addSpotMesh(options, "game").enablePubSub("inproc://c1876-spot-pub");
        options.validate();

        ZLinkSpotRuntime runtime =
                new ZLinkSpotRuntime(
                        probe.provider(),
                        new ZLinkBackendAdapterOptions(Duration.ofSeconds(5)),
                        options.registration(),
                        null,
                        new ZLinkStringMessageSerializer(),
                        ZLinkHandlerActivator.services(),
                        null);
        var field = ZLinkSpotRuntime.class.getDeclaredField("publishers");
        field.setAccessible(true);
        ZLinkSpotPublisherRuntime publishers = (ZLinkSpotPublisherRuntime) field.get(runtime);

        var executorField = ZLinkSpotPublisherRuntime.class.getDeclaredField("multicastExecutor");
        executorField.setAccessible(true);
        ((ThreadPoolExecutor) executorField.get(publishers)).shutdown();
        CompletableFuture<Void> waiting = new CompletableFuture<>();
        ThreadPoolExecutor executor =
                new ThreadPoolExecutor(1, 1, 0, TimeUnit.SECONDS, new LinkedBlockingQueue<>()) {
                    @Override
                    public boolean awaitTermination(long timeout, TimeUnit unit)
                            throws InterruptedException {
                        waiting.complete(null);
                        return super.awaitTermination(timeout, unit);
                    }
                };
        executorField.set(publishers, executor);
        CompletableFuture<Void> closing = null;
        try {
            publishers
                    .submitAsync(
                            "game",
                            "game",
                            "topic",
                            Message.from("payload".getBytes(StandardCharsets.UTF_8)),
                            Optional.empty(),
                            ZLinkApplicationMetadata.empty())
                    .toCompletableFuture()
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertTrue(entered.await(WAIT_SECONDS, TimeUnit.SECONDS));
            closing = runtime.closeAsync().toCompletableFuture();
            CompletableFuture.anyOf(waiting, probe.nodeClosing, closing)
                    .get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertFalse(closing.isDone(), "close completed while a committed publish was held");
            assertFalse(probe.nodeClosed.get(), "SpotNode closed while publish in flight");
            assertFalse(probe.contextClosed.get());
        } finally {
            release.countDown();
            if (closing != null) closing.get(WAIT_SECONDS, TimeUnit.SECONDS);
            else runtime.closeAsync().toCompletableFuture().get(WAIT_SECONDS, TimeUnit.SECONDS);
            assertTrue(executor.awaitTermination(WAIT_SECONDS, TimeUnit.SECONDS));
        }
        assertTrue(probe.nodeClosed.get());
        assertTrue(probe.contextClosed.get());
        events.forEach(event -> System.out.println("[c1876-spot] " + event));

        assertFalse(probe.interrupted.get(), "shutdown interrupted a committed publish");
        assertEquals(
                0, probe.nodeClosedWhileInFlight.get(), "SpotNode closed while publish in flight");
        assertEquals(
                0,
                probe.contextClosedWhileInFlight.get(),
                "owned context closed while publish in flight");
        assertEquals(
                0, probe.useAfterClose.get(), "released publish touched a closed node/context");
    }

    private static final class Probe {
        private final List<String> events;
        private final CountDownLatch entered;
        private final CountDownLatch release;
        final CompletableFuture<Void> nodeClosing = new CompletableFuture<>();
        final AtomicBoolean interrupted = new AtomicBoolean();
        final AtomicBoolean inFlight = new AtomicBoolean();
        final AtomicBoolean nodeClosed = new AtomicBoolean();
        final AtomicBoolean contextClosed = new AtomicBoolean();
        final AtomicInteger nodeClosedWhileInFlight = new AtomicInteger();
        final AtomicInteger contextClosedWhileInFlight = new AtomicInteger();
        final AtomicInteger useAfterClose = new AtomicInteger();

        Probe(List<String> events, CountDownLatch entered, CountDownLatch release) {
            this.events = events;
            this.entered = entered;
            this.release = release;
        }

        ZLinkBackendAdapterProvider provider() {
            ZLinkBackendContext context = context();
            ZLinkInternalSpotNode node = node();
            return new ZLinkBackendAdapterProvider() {
                @Override
                public ZLinkChannelBackendAdapter createChannelAdapter(
                        ZLinkBackendAdapterOptions options) {
                    return new ZLinkChannelBackendAdapter() {
                        @Override
                        public ZLinkBackendContext createContext() {
                            return context;
                        }

                        @Override
                        public ZLinkBackendDealerSocket createDealerSocket(
                                ZLinkBackendContext ignored) {
                            throw new UnsupportedOperationException();
                        }

                        @Override
                        public ZLinkBackendRouterSocket createRouterSocket(
                                ZLinkBackendContext ignored) {
                            throw new UnsupportedOperationException();
                        }

                        @Override
                        public ZLinkBackendPublisherSocket createPublisherSocket(
                                ZLinkBackendContext ignored, Duration sendTimeout) {
                            throw new UnsupportedOperationException();
                        }

                        @Override
                        public ZLinkBackendSubscriberSocket createSubscriberSocket(
                                ZLinkBackendContext ignored) {
                            throw new UnsupportedOperationException();
                        }
                    };
                }

                @Override
                public ZLinkSpotBackendAdapter createSpotAdapter(
                        ZLinkBackendAdapterOptions options) {
                    return (ignoredContext, ignoredMode) -> node;
                }

                @Override
                public ZLinkStreamBackendAdapter createStreamAdapter(
                        ZLinkBackendAdapterOptions options) {
                    throw new UnsupportedOperationException();
                }

                @Override
                public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                        ZLinkBackendAdapterOptions options) {
                    throw new UnsupportedOperationException();
                }
            };
        }

        private ZLinkBackendContext context() {
            return new ZLinkBackendContext() {
                @Override
                public String name() {
                    return "context";
                }

                @Override
                public void shutdown() {
                    events.add("native: context.shutdown, publish in flight=" + inFlight.get());
                }

                @Override
                public void close() {
                    boolean busy = inFlight.get();
                    if (busy) contextClosedWhileInFlight.incrementAndGet();
                    contextClosed.set(true);
                    events.add("native: context.close, publish in flight=" + busy);
                }
            };
        }

        private final Object entrySpot =
                stub(systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot.class);

        private static Object stub(Class<?> type) {
            InvocationHandler handler =
                    (proxy, method, arguments) ->
                            switch (method.getName()) {
                                case "spotId", "name" -> "entry-spot";
                                default -> {
                                    if (method.isDefault()) {
                                        yield InvocationHandler.invokeDefault(
                                                proxy, method, arguments);
                                    }
                                    yield defaultValue(method);
                                }
                            };
            return Proxy.newProxyInstance(type.getClassLoader(), new Class<?>[] {type}, handler);
        }

        private ZLinkInternalSpotNode node() {
            InvocationHandler handler =
                    (proxy, method, arguments) ->
                            switch (method.getName()) {
                                case "publish" -> {
                                    publish();
                                    yield null;
                                }
                                case "close" -> {
                                    boolean busy = inFlight.get();
                                    if (busy) nodeClosedWhileInFlight.incrementAndGet();
                                    nodeClosed.set(true);
                                    events.add("native: spotNode.close, publish in flight=" + busy);
                                    nodeClosing.complete(null);
                                    yield null;
                                }
                                case "name" -> "spotNode";
                                case "entrySpot" -> entrySpot;
                                case "instanceActivationTerminals" ->
                                        CompletableFuture.completedFuture(null);
                                default -> {
                                    if (method.isDefault()) {
                                        yield InvocationHandler.invokeDefault(
                                                proxy, method, arguments);
                                    }
                                    yield defaultValue(method);
                                }
                            };
            return (ZLinkInternalSpotNode)
                    Proxy.newProxyInstance(
                            ZLinkInternalSpotNode.class.getClassLoader(),
                            new Class<?>[] {ZLinkInternalSpotNode.class},
                            handler);
        }

        private void publish() {
            // Models the window inside the native SpotNode publish call.
            inFlight.set(true);
            events.add("binding: SpotNode.publish entered on " + Thread.currentThread().getName());
            entered.countDown();
            boolean interrupted = false;
            while (true) {
                try {
                    release.await();
                    break;
                } catch (InterruptedException ignored) {
                    interrupted = true;
                    this.interrupted.set(true);
                }
            }
            boolean node = nodeClosed.get();
            boolean ctx = contextClosed.get();
            if (node || ctx) useAfterClose.incrementAndGet();
            events.add(
                    "binding: SpotNode.publish resumed, node closed="
                            + node
                            + ", context closed="
                            + ctx
                            + ", interrupted while held="
                            + interrupted);
            inFlight.set(false);
            if (interrupted) Thread.currentThread().interrupt();
        }

        private static Object defaultValue(Method method) {
            Class<?> type = method.getReturnType();
            if (CompletionStage.class.isAssignableFrom(type)) {
                return CompletableFuture.completedFuture(null);
            }
            if (type == Optional.class) return Optional.empty();
            if (type == List.class) return List.of();
            if (!type.isPrimitive()) return null;
            if (type == boolean.class) return false;
            if (type == void.class) return null;
            if (type == long.class) return 0L;
            if (type == char.class) return '\0';
            if (type == double.class) return 0d;
            if (type == float.class) return 0f;
            if (type == short.class) return (short) 0;
            if (type == byte.class) return (byte) 0;
            return 0;
        }
    }
}
