package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkMessageFlowLogMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.configuration.ZLinkDispatchOptionsRegistration;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationContext;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceCompletionDispatcher;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationRegistry;
import systems.zlink.framework.runtime.internal.spots.SpotTerminalProbe;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import java.lang.reflect.Proxy;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

final class ZLinkSpotDirectCompletionDispatchTest {
    private static final Duration REGISTRY_TIMEOUT = Duration.ofHours(1);
    private static final long WAIT_SECONDS = 5;

    @Test
    void sameSpotRequestCanYieldButCannotHoldTheGate() {
        var submissions = new AtomicInteger();
        var sends = new AtomicInteger();
        var response = new CompletableFuture<ZLinkBackendReceived>();
        var backend =
                (ZLinkBackendSpot)
                        Proxy.newProxyInstance(
                                ZLinkBackendSpot.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendSpot.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "rememberSpotAuthority" -> null;
                                            case "requestToSpot" -> {
                                                submissions.incrementAndGet();
                                                yield response;
                                            }
                                            case "sendToSpot" -> {
                                                sends.incrementAndGet();
                                                yield CompletableFuture.completedFuture(null);
                                            }
                                            default -> throw new AssertionError(method.getName());
                                        });
        var resolver = new SpotTerminalProbe(null, false);
        var outbound =
                new DefaultSpotOutbound(
                        backend,
                        "mesh",
                        null,
                        new ZLinkStringMessageSerializer(),
                        ignored -> ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        outbound(),
                        null,
                        null,
                        false,
                        Duration.ofSeconds(WAIT_SECONDS),
                        () -> resolver,
                        null);
        var execution =
                new ZLinkSuspendInvocationContext.ApplicationExecution(
                        "spot", null, true, true, null);
        var worker = new ZLinkManualExecutor();
        var queue = new ZLinkSerialExecutionQueue(worker, ZLinkExecutionLanePolicy.spot());
        var released = new CountDownLatch(1);
        try {
            var turn =
                    queue.enqueue(
                            () -> {
                                try (var ignored =
                                        ZLinkSuspendInvocationContext.enterApplicationExecution(
                                                execution)) {
                                    var failure =
                                            assertThrows(
                                                    ZLinkFrameworkException.class,
                                                    () ->
                                                            outbound.requestToSpot(
                                                                            "spot", "request")
                                                                    .submit(String.class));
                                    assertEquals(
                                            ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                            failure.kind());
                                    assertEquals(0, submissions.get());
                                    var notice = outbound.sendToSpot("spot", "notice").submit();
                                    return outbound.requestToSpot("spot", "request")
                                            .yield(String.class)
                                            .thenCombine(
                                                    notice,
                                                    (value, sent) -> {
                                                        assertEquals("reply", value);
                                                        return null;
                                                    });
                                }
                            },
                            null);
            var sibling =
                    queue.enqueue(
                            () -> {
                                assertEquals(1, submissions.get());
                                response.complete(reply(released));
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
            worker.drainUntil(turn);
            worker.drainUntil(sibling);
            assertEquals(1, sends.get());
            assertEquals(1, submissions.get());
            assertEquals(0, released.getCount());
        } finally {
            queue.close();
        }
    }

    @Test
    void exceptionalBackendEmitsOnePublicTerminalWithErrorDetails() {
        systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence.awaitIdle();
        var records = new java.util.concurrent.CopyOnWriteArrayList<String>();
        var logger = java.util.logging.Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var handler =
                new java.util.logging.Handler() {
                    @Override
                    public void publish(java.util.logging.LogRecord record) {
                        records.add(record.getMessage());
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        var oldLevel = logger.getLevel();
        logger.setLevel(java.util.logging.Level.ALL);
        logger.addHandler(handler);
        try {
            var options = new ZLinkDispatchOptionsRegistration();
            options.messageFlow(ZLinkMessageFlowLogMode.NORMAL);
            var outbound =
                    new ZLinkSpotDirectOutbound(
                            new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer()),
                            new ZLinkMessageFlowTracer(
                                    options, ZLinkHandlerActivator.reflection(), Runnable::run));
            var failure = new IllegalStateException("connection admission failed");
            try (var payload = message("request")) {
                var result = request(outbound, CompletableFuture.failedFuture(failure), payload);
                assertSame(
                        failure,
                        assertThrows(
                                        java.util.concurrent.CompletionException.class,
                                        () -> result.toCompletableFuture().join())
                                .getCause());
            }
            systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryTestFence.awaitIdle();
            var terminals =
                    records.stream()
                            .filter(
                                    record ->
                                            record.contains("connection admission failed")
                                                    && record.contains("reply_received"))
                            .toList();
            assertEquals(1, terminals.size(), records.toString());
            assertTrue(
                    terminals.getFirst().contains("error_type=IllegalStateException"),
                    terminals.toString());
            assertTrue(terminals.getFirst().contains(" flow="), terminals.toString());
        } finally {
            logger.removeHandler(handler);
            logger.setLevel(oldLevel);
        }
    }

    @Test
    void remoteReplyReusesTheRegisteredCompletionTurn() throws Exception {
        try (var registry = new ZLinkServiceOperationRegistry()) {
            var operation =
                    ZLinkCompletionBridge.await(
                            registry.<ZLinkBackendReceived>register(REGISTRY_TIMEOUT));
            var outbound = outbound();
            var observed = new CountDownLatch(1);
            var closed = new CountDownLatch(1);
            var completionContext = new AtomicBoolean();
            var virtual = new AtomicBoolean();
            var value = new AtomicReference<String>();
            var origin = new AtomicReference<Thread>();
            var callerThread = new AtomicReference<Thread>();
            var backendCallbacks = new AtomicInteger();
            try (Message payload = message("request")) {
                var caller =
                        request(
                                outbound,
                                observedBackend(
                                        operation.completion(), origin, backendCallbacks, observed),
                                payload);
                caller.whenComplete(
                        (reply, failure) -> {
                            value.set(reply);
                            callerThread.set(Thread.currentThread());
                            completionContext.set(
                                    ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                            virtual.set(Thread.currentThread().isVirtual());
                            observed.countDown();
                        });
                assertTrue(registry.complete(operation.id(), reply(closed)));
                assertTrue(observed.await(WAIT_SECONDS, TimeUnit.SECONDS));
                assertTrue(closed.await(WAIT_SECONDS, TimeUnit.SECONDS));
                assertEquals("reply", value.get());
                assertEquals("reply", caller.toCompletableFuture().join());
                assertEquals(1, backendCallbacks.get());
                assertSame(
                        origin.get(),
                        callerThread.get(),
                        "caller must reuse the backend completion resource");
                assertTrue(
                        completionContext.get(),
                        "caller must reuse the registered completion turn");
                assertFalse(virtual.get(), "Java completion uses a platform worker");
            }
        }
    }

    @Test
    void localReplyLeavesItsOwnerTurnBeforeCallingTheCaller() throws Exception {
        try (var ownerExecutor = Executors.newSingleThreadExecutor()) {
            var owner = new ZLinkStateLane(ownerExecutor);
            var backend = new CompletableFuture<ZLinkBackendReceived>();
            var observed = new CountDownLatch(1);
            var closed = new CountDownLatch(1);
            var completionContext = new AtomicBoolean();
            var ownerContext = new AtomicBoolean();
            var virtual = new AtomicBoolean();
            try (Message payload = message("request")) {
                var caller = request(outbound(), backend, payload);
                caller.whenComplete(
                        (reply, failure) -> {
                            completionContext.set(
                                    ZLinkServiceCompletionDispatcher.isCurrentCompletion());
                            ownerContext.set(owner.isOnLane());
                            virtual.set(Thread.currentThread().isVirtual());
                            observed.countDown();
                        });
                owner.runAsync(() -> backend.complete(reply(closed))).toCompletableFuture().join();
                assertTrue(observed.await(WAIT_SECONDS, TimeUnit.SECONDS));
                assertTrue(closed.await(WAIT_SECONDS, TimeUnit.SECONDS));
                assertEquals("reply", caller.toCompletableFuture().join());
                assertTrue(completionContext.get());
                assertFalse(ownerContext.get());
                assertFalse(virtual.get());
            }
        }
    }

    @Test
    void failureAndCancellationKeepTheirTerminalAndReleaseTheReply() throws Exception {
        var failure = new IllegalStateException("backend failure");
        var backend = new CompletableFuture<ZLinkBackendReceived>();
        try (Message payload = message("request")) {
            var caller = request(outbound(), backend, payload);
            backend.completeExceptionally(failure);
            assertSame(
                    failure,
                    assertThrows(
                                    java.util.concurrent.CompletionException.class,
                                    () -> caller.toCompletableFuture().join())
                            .getCause());
        }
        var pending = new CompletableFuture<ZLinkBackendReceived>();
        var closed = new CountDownLatch(1);
        try (Message payload = message("request")) {
            var caller = request(outbound(), pending, payload).toCompletableFuture();
            assertTrue(caller.cancel(false));
            pending.complete(reply(closed));
            assertTrue(closed.await(WAIT_SECONDS, TimeUnit.SECONDS));
            assertTrue(caller.isCancelled());
        }
    }

    @SuppressWarnings("unchecked")
    private static CompletionStage<ZLinkBackendReceived> observedBackend(
            CompletionStage<ZLinkBackendReceived> source,
            AtomicReference<Thread> origin,
            AtomicInteger callbacks,
            CountDownLatch observed) {
        return (CompletionStage<ZLinkBackendReceived>)
                Proxy.newProxyInstance(
                        CompletionStage.class.getClassLoader(),
                        new Class<?>[] {CompletionStage.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("whenComplete")) {
                                var callback =
                                        (BiConsumer<ZLinkBackendReceived, Throwable>) arguments[0];
                                return source.whenComplete(
                                        (reply, failure) -> {
                                            callbacks.incrementAndGet();
                                            origin.set(Thread.currentThread());
                                            callback.accept(reply, failure);
                                            try {
                                                // Keep the source worker occupied until the public
                                                // callback ran.
                                                // A different dispatcher turn cannot reuse this
                                                // worker by chance.
                                                if (!observed.await(WAIT_SECONDS, TimeUnit.SECONDS))
                                                    throw new AssertionError(
                                                            "caller callback did not run");
                                            } catch (InterruptedException interrupted) {
                                                Thread.currentThread().interrupt();
                                                throw new AssertionError(interrupted);
                                            }
                                        });
                            }
                            return method.invoke(source, arguments);
                        });
    }

    private static CompletionStage<String> request(
            ZLinkSpotDirectOutbound outbound,
            CompletionStage<ZLinkBackendReceived> backend,
            Message payload) {
        var spot =
                (ZLinkBackendSpot)
                        Proxy.newProxyInstance(
                                ZLinkBackendSpot.class.getClassLoader(),
                                new Class<?>[] {ZLinkBackendSpot.class},
                                (proxy, method, arguments) -> {
                                    if (method.getName().equals("requestToSpot")) return backend;
                                    throw new UnsupportedOperationException(method.getName());
                                });
        return systems.zlink.framework.execution.ZLinkSerialExecutionQueue.manageCurrent(
                outbound.submitRequest(
                        spot,
                        RoutingId.from("target"),
                        "spot",
                        1L,
                        0L,
                        payload,
                        Optional.empty(),
                        null,
                        systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata.empty(),
                        Duration.ZERO,
                        String.class,
                        new CompletableFuture<>()));
    }

    private static ZLinkSpotDirectOutbound outbound() {
        var options = new ZLinkDispatchOptionsRegistration();
        options.messageFlow(ZLinkMessageFlowLogMode.OFF);
        return new ZLinkSpotDirectOutbound(
                new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer()),
                new ZLinkMessageFlowTracer(
                        options, ZLinkHandlerActivator.reflection(), Runnable::run));
    }

    private static ZLinkBackendReceived reply(CountDownLatch closed) {
        return new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.empty(),
                Optional.empty(),
                Optional.empty(),
                new byte[0],
                List.of(message("reply")),
                null,
                closed::countDown);
    }

    private static Message message(String value) {
        return Message.from(value.getBytes(StandardCharsets.UTF_8));
    }
}
