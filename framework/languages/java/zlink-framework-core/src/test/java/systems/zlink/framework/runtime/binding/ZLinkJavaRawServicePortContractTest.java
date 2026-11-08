package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.Context;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.errors.CloseResult;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.eventing.Poller;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.ReplyOperation;
import systems.zlink.contracts.messaging.ReplySubmitOperation;
import systems.zlink.contracts.messaging.ReplyToken;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;

final class ZLinkJavaRawServicePortContractTest {
    @Test
    void tokenlessRequestCapacityFailureRetainsItsKindAndCauseThroughMeshCompletion()
            throws Exception {
        var refusal =
                new ZlinkSubmitException(
                        systems.zlink.contracts.sockets.SubmitResult.BACKPRESSURED, 11);
        var submit =
                (systems.zlink.contracts.messaging.RequestSubmitOperation)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.messaging.RequestSubmitOperation.class
                                        .getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.contracts.messaging.RequestSubmitOperation.class
                                },
                                (proxy, method, args) -> {
                                    if (method.getName().equals("submit")) throw refusal;
                                    if (method.getName().equals("timeout")) return proxy;
                                    throw new AssertionError(method.getName());
                                });
        var request =
                (systems.zlink.contracts.messaging.RequestOperation)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.messaging.RequestOperation.class
                                        .getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.contracts.messaging.RequestOperation.class
                                },
                                (proxy, method, args) -> {
                                    assertEquals("message", method.getName());
                                    return submit;
                                });
        RouterSocket router =
                (RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            case "equals" -> proxy == args[0];
                                            case "setRoutingId", "close" -> null;
                                            case "request" -> request;
                                            default -> throw new AssertionError(method.getName());
                                        });
        var context =
                (systems.zlink.contracts.core.Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.core.Context.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.core.Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        try (var port = new ZLinkJavaRawServicePort(context)) {
            var target = RoutingId.from("capacity-target");
            var caller = port.openRouter(RoutingId.from("capacity-source"));
            var submitted =
                    port.requestMessages(
                            caller,
                            target,
                            List.of(Message.from(new byte[] {1})),
                            Duration.ofSeconds(1),
                            ignored -> {
                                throw new AssertionError("capacity refusal has no reply");
                            },
                            ignored -> {},
                            null,
                            null);
            var sourceError =
                    assertThrows(
                                    ExecutionException.class,
                                    () -> submitted.toCompletableFuture().get(1, TimeUnit.SECONDS))
                            .getCause();
            assertEquals(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    ZLinkJavaRawMeshNode.backendResult(
                                    ZLinkJavaRawMeshNode.requestTerminal(sourceError))
                            .toFrameworkErrorKind());
            Function<
                            Throwable,
                            systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived>
                    decodeFailure =
                            failure ->
                                    new systems.zlink.framework.runtime.internal.backend
                                            .ZLinkBackendReceived(
                                            ZLinkJavaRawMeshNode.backendResult(
                                                    ZLinkJavaRawMeshNode.requestTerminal(failure)),
                                            Optional.of(target),
                                            Optional.empty(),
                                            Optional.empty(),
                                            List.of());
            // The mesh completion path reports the terminal with the same failure projection.
            var mapped =
                    new CompletableFuture<
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendReceived>();
            port.requestMessagesTo(
                    caller,
                    target,
                    List.of(Message.from(new byte[] {1})),
                    Duration.ofSeconds(1),
                    ignored -> {
                        throw new AssertionError("capacity refusal has no reply");
                    },
                    decodeFailure,
                    (value, failure) -> {
                        if (failure != null) mapped.completeExceptionally(failure);
                        else mapped.complete(value);
                    },
                    null,
                    null);
            var error =
                    assertThrows(
                                    ExecutionException.class,
                                    () -> mapped.toCompletableFuture().get(1, TimeUnit.SECONDS))
                            .getCause();
            var framework = assertInstanceOf(ZLinkFrameworkException.class, error);
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, framework.kind());
            assertTrue(framework.getMessage().contains("submission capacity is unavailable"));
            assertSame(refusal, framework.getCause());
        }
    }

    @Test
    void socketRegistrationRunsWithTheActualStateOwner() {
        var stopped = new IllegalStateException("creation stopped before native allocation");
        var context =
                (systems.zlink.contracts.core.Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.core.Context.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.core.Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    org.junit.jupiter.api.Assertions.assertNotNull(
                                            systems.zlink.framework.runtime.internal.execution
                                                    .ZLinkStateLane.current());
                                    throw stopped;
                                });
        try (var port = new ZLinkJavaRawServicePort(context)) {
            assertSame(
                    stopped,
                    assertThrows(
                            IllegalStateException.class,
                            () -> port.openRouter(RoutingId.from("lane-owner"))));
        }
    }

    @Test
    void failedPollerCloseCanBeRetriedByItsOwner() throws Exception {
        var attempts = new AtomicInteger();
        var failure = new ZlinkCloseException(CloseResult.BUSY, 16);
        try (var context = systems.zlink.contracts.core.Zlink.createContext();
                var router = context.createRouterSocket()) {
            var poller =
                    (systems.zlink.contracts.eventing.Poller)
                            java.lang.reflect.Proxy.newProxyInstance(
                                    systems.zlink.contracts.eventing.Poller.class.getClassLoader(),
                                    new Class<?>[] {systems.zlink.contracts.eventing.Poller.class},
                                    (proxy, method, args) -> {
                                        assertEquals("close", method.getName());
                                        if (attempts.incrementAndGet() == 1) {
                                            throw failure;
                                        }
                                        return null;
                                    });
            var owner =
                    new ZLinkJavaSocketReceivePoller(router, true, false, () -> poller, () -> null);
            assertSame(failure, assertThrows(failure.getClass(), owner::close));
            owner.close();
            owner.close();
            assertEquals(2, attempts.get());
        }
    }

    @Test
    void failedSocketCloseRetainsOwnershipUntilLaterCleanupSucceeds() throws Exception {
        var attempts = new AtomicInteger();
        var failure = new ZlinkCloseException(CloseResult.BUSY, 16);
        RouterSocket router =
                (RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) -> {
                                    return switch (method.getName()) {
                                        case "hashCode" -> System.identityHashCode(proxy);
                                        case "equals" -> proxy == args[0];
                                        case "setRoutingId" -> null;
                                        case "close" -> {
                                            org.junit.jupiter.api.Assertions.assertNull(
                                                    systems.zlink.framework.runtime.internal
                                                            .execution.ZLinkStateLane.current());
                                            if (attempts.incrementAndGet()
                                                    <= ZLinkJavaRawServicePort
                                                            .CLOSE_BUSY_MAX_ATTEMPTS) {
                                                throw failure;
                                            }
                                            yield null;
                                        }
                                        default -> throw new AssertionError(method.getName());
                                    };
                                });
        var context =
                (systems.zlink.contracts.core.Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.core.Context.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.core.Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        try (var port = new ZLinkJavaRawServicePort(context)) {
            port.openRouter(RoutingId.from("close-busy"));
            assertSame(failure, assertThrows(failure.getClass(), port::close));
            port.close();
            assertEquals(ZLinkJavaRawServicePort.CLOSE_BUSY_MAX_ATTEMPTS + 1, attempts.get());
            port.close();
            assertEquals(ZLinkJavaRawServicePort.CLOSE_BUSY_MAX_ATTEMPTS + 1, attempts.get());
        }
    }

    @Test
    void busySocketCloseSucceedsWhenBusyClearsWithinTheCurrentCloseAttempt() throws Exception {
        var attempts = new AtomicInteger();
        var failure = new ZlinkCloseException(CloseResult.BUSY, 16);
        RouterSocket router =
                (RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) -> {
                                    return switch (method.getName()) {
                                        case "hashCode" -> System.identityHashCode(proxy);
                                        case "equals" -> proxy == args[0];
                                        case "setRoutingId" -> null;
                                        case "close" -> {
                                            if (attempts.incrementAndGet() < 3) throw failure;
                                            yield null;
                                        }
                                        default -> throw new AssertionError(method.getName());
                                    };
                                });
        var context =
                (Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Context.class.getClassLoader(),
                                new Class<?>[] {Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        try (var port = new ZLinkJavaRawServicePort(context)) {
            port.openRouter(RoutingId.from("close-busy-clears"));
            port.close();
            assertEquals(3, attempts.get());
        }
    }

    @Test
    void meshExplicitCloseReachesRetainedSocketOwner() throws Exception {
        var fixture = meshWithSocketClose(true);
        var reentrantCalls = new AtomicInteger();
        fixture.onSocketClose()
                .set(
                        () -> {
                            reentrantCalls.incrementAndGet();
                            fixture.mesh().close();
                        });
        var lastClose = closeOnDaemon(fixture.mesh());
        try {
            assertSame(fixture.failure(), lastClose.get(2, TimeUnit.SECONDS));
            assertEquals(
                    systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState.STOPPED,
                    fixture.mesh().status().state());
            assertThrows(IllegalStateException.class, fixture.mesh()::start);
            assertEquals(ZLinkJavaRawServicePort.CLOSE_BUSY_MAX_ATTEMPTS, fixture.attempts().get());
            assertEquals(1, reentrantCalls.get());

            lastClose = closeOnDaemon(fixture.mesh());
            assertNull(lastClose.get(2, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkJavaRawServicePort.CLOSE_BUSY_MAX_ATTEMPTS + 1, fixture.attempts().get());
            assertEquals(2, reentrantCalls.get());
            lastClose = closeOnDaemon(fixture.mesh());
            assertNull(lastClose.get(2, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkJavaRawServicePort.CLOSE_BUSY_MAX_ATTEMPTS + 1, fixture.attempts().get());
            assertEquals(2, reentrantCalls.get());
        } finally {
            // A timed-out daemon is still inside close; avoid joining its in-flight port attempt.
            if (lastClose.isDone()) fixture.port().close();
        }
    }

    private static CompletableFuture<Throwable> closeOnDaemon(ZLinkJavaRawMeshNode mesh) {
        var result = new CompletableFuture<Throwable>();
        Thread.ofPlatform()
                .daemon()
                .start(
                        () -> {
                            try {
                                mesh.close();
                                result.complete(null);
                            } catch (Throwable failure) {
                                result.complete(failure);
                            }
                        });
        return result;
    }

    @Test
    void concurrentMeshCloseWaitsForFirstLogicalCleanup() throws Exception {
        var fixture = meshWithSocketClose(false);
        var cleanupEntered = new CountDownLatch(1);
        var releaseCleanup = new CountDownLatch(1);
        fixture.afterLogicalCleanupTurn()
                .set(
                        () -> {
                            cleanupEntered.countDown();
                            try {
                                releaseCleanup.await();
                            } catch (InterruptedException failure) {
                                Thread.currentThread().interrupt();
                                throw new IllegalStateException(failure);
                            }
                        });
        try (var port = fixture.port();
                var callers = Executors.newThreadPerTaskExecutor(Thread.ofVirtual().factory())) {
            try {
                var first = callers.submit(fixture.mesh()::close);
                assertTrue(cleanupEntered.await(2, TimeUnit.SECONDS));
                var secondStarted = new CountDownLatch(1);
                var second =
                        callers.submit(
                                () -> {
                                    secondStarted.countDown();
                                    fixture.mesh().close();
                                });
                assertTrue(secondStarted.await(2, TimeUnit.SECONDS));
                assertEquals(0, fixture.attempts().get());
                assertThrows(TimeoutException.class, () -> second.get(100, TimeUnit.MILLISECONDS));
                releaseCleanup.countDown();
                first.get(2, TimeUnit.SECONDS);
                second.get(2, TimeUnit.SECONDS);
                assertEquals(1, fixture.attempts().get());
            } finally {
                releaseCleanup.countDown();
            }
        }
    }

    @Test
    void closeCallbackOnLogicalOwnerCanReenterClose() throws Exception {
        var fixture = meshWithSocketClose(false);
        var callbackReturned = new CountDownLatch(1);
        fixture.afterLogicalCleanupTurn()
                .set(
                        () -> {
                            fixture.mesh().close();
                            callbackReturned.countDown();
                        });
        var outerClose = new CompletableFuture<Void>();
        try (var port = fixture.port()) {
            Thread.ofPlatform()
                    .daemon()
                    .start(
                            () -> {
                                try {
                                    fixture.mesh().close();
                                    outerClose.complete(null);
                                } catch (Throwable failure) {
                                    outerClose.completeExceptionally(failure);
                                }
                            });
            assertTrue(callbackReturned.await(2, TimeUnit.SECONDS));
            outerClose.get(2, TimeUnit.SECONDS);
            assertEquals(1, fixture.attempts().get());
        }
    }

    private static MeshCloseFixture meshWithSocketClose(boolean failFirst) {
        var attempts = new AtomicInteger();
        var onSocketClose = new AtomicReference<Runnable>();
        var afterLogicalCleanupTurn = new AtomicReference<Runnable>();
        var failure = new ZlinkCloseException(CloseResult.BUSY, 16);
        RouterSocket router =
                (RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "hashCode" -> System.identityHashCode(proxy);
                                            case "equals" -> proxy == args[0];
                                            case "setRoutingId" -> null;
                                            case "close" -> {
                                                int attempt = attempts.incrementAndGet();
                                                // Re-enter once per port close() call: a failing
                                                // close spends every BUSY retry before returning.
                                                boolean firstOfCall =
                                                        !failFirst
                                                                || (attempt - 1)
                                                                                % ZLinkJavaRawServicePort
                                                                                        .CLOSE_BUSY_MAX_ATTEMPTS
                                                                        == 0;
                                                var callback =
                                                        firstOfCall ? onSocketClose.get() : null;
                                                if (callback != null) callback.run();
                                                if (failFirst
                                                        && attempt
                                                                <= ZLinkJavaRawServicePort
                                                                        .CLOSE_BUSY_MAX_ATTEMPTS)
                                                    throw failure;
                                                yield null;
                                            }
                                            default -> throw new AssertionError(method.getName());
                                        });
        var context =
                (systems.zlink.contracts.core.Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                systems.zlink.contracts.core.Context.class.getClassLoader(),
                                new Class<?>[] {systems.zlink.contracts.core.Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        var lane =
                new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane(
                        command -> {
                            command.run();
                            // Run after the lane is free, while its submitting close still owns
                            // cleanup.
                            var callback = afterLogicalCleanupTurn.getAndSet(null);
                            if (callback != null) callback.run();
                        });
        var port = new ZLinkJavaRawServicePort(context, lane);
        port.openRouter(RoutingId.from("mesh-close-busy"));
        var mesh =
                new ZLinkJavaRawMeshNode(
                        "mesh",
                        System::currentTimeMillis,
                        System::nanoTime,
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        port,
                        Runnable::run,
                        null,
                        null,
                        null,
                        Map.of());
        return new MeshCloseFixture(
                port, mesh, attempts, failure, onSocketClose, afterLogicalCleanupTurn);
    }

    private record MeshCloseFixture(
            ZLinkJavaRawServicePort port,
            ZLinkJavaRawMeshNode mesh,
            AtomicInteger attempts,
            ZlinkCloseException failure,
            AtomicReference<Runnable> onSocketClose,
            AtomicReference<Runnable> afterLogicalCleanupTurn) {}

    @Test
    void oneOrderedRegistryOwnsSocketsAndRejectsForeignSockets() throws Exception {
        try (ZLinkJavaRawServicePort owner = new ZLinkJavaRawServicePort();
                ZLinkJavaRawServicePort foreign = new ZLinkJavaRawServicePort()) {
            var first = owner.openRouter(RoutingId.from("owner-first"));
            var second = owner.openRouter(RoutingId.from("owner-second"));
            var other = foreign.openRouter(RoutingId.from("owner-first"));
            var registry = owner.receivePollers;
            assertEquals(List.of(second, first), List.copyOf(registry.reversed().keySet()));
            assertFalse(
                    Arrays.stream(ZLinkJavaRawServicePort.class.getDeclaredFields())
                            .anyMatch(field -> List.class.isAssignableFrom(field.getType())),
                    "socket ownership must not also be kept in a linear list");
            assertThrows(IllegalArgumentException.class, () -> owner.receiveNow(other));
            assertThrows(
                    IllegalArgumentException.class,
                    () -> owner.send(other, RoutingId.from("target"), List.of(new byte[] {1})));
            owner.close();
            assertTrue(registry.isEmpty());
            assertThrows(IllegalStateException.class, () -> owner.receiveNow(first));
        }
    }

    @Test
    void requestDecoderReadsNativeReplyBeforeThePortClosesItsOwner() throws Exception {
        RoutingId callerId = RoutingId.from("native-reply-caller");
        RoutingId targetId = RoutingId.from("native-reply-target");
        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort()) {
            var target = port.openRouter(targetId);
            var caller = port.openRouter(callerId);
            port.ensureReceivePollerRegistered(caller);
            String endpoint = "inproc://native-reply-" + System.nanoTime();
            target.bind(endpoint);
            caller.connect(endpoint);
            var reply =
                    port.request(
                                    caller,
                                    targetId,
                                    List.of(new byte[] {1}),
                                    Duration.ofSeconds(2),
                                    parts -> {
                                        assertEquals(1, parts.size());
                                        var view = parts.getFirst().dataBuffer();
                                        assertTrue(view.isDirect());
                                        assertEquals(3, view.remaining());
                                        return view.get(0) + view.get(1) + view.get(2);
                                    },
                                    null)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var incoming = port.receiveNow(target)) {
                assertNotNull(incoming);
                Message original = incoming.received().parts().getFirst();
                // The byte-array control view is lazy and shares the receive owner.
                original.mutableDataBuffer().put(0, (byte) 7);
                assertArrayEquals(new byte[] {7}, incoming.frames().getFirst());
                port.reply(
                        target,
                        incoming.source(),
                        incoming.requestSequence(),
                        List.of(new byte[] {2, 3, 4}));
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            assertEquals(9, reply.get(2, TimeUnit.SECONDS));
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false,false",
        "false,true,false",
        "true,false,false",
        "true,true,false",
        "false,false,true",
        "false,true,true",
        "true,false,true",
        "true,true,true"
    })
    void durableLogicalTerminalRetainsTheNativeReplyReceiver(
            boolean cancel, boolean codecThrows, boolean initiallyReady) throws Exception {
        var decoded = new AtomicInteger();
        var attempts = new AtomicInteger();
        var observerClosed = new CompletableFuture<Void>();
        var ready = new AtomicBoolean(initiallyReady);
        var observations = new AtomicInteger();
        var physical = new AtomicReference<CompletionStage<?>>();
        var payload = new AtomicReference<Message>();
        var codecFailure = new IllegalStateException("controlled codec failure");
        try (var port = new ZLinkJavaRawServicePort();
                var operations =
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceOperationRegistry(
                                new IllegalStateException("closed"))) {
            var targetId = RoutingId.from("durable-native-target");
            var target = port.openRouter(targetId);
            var caller = port.openRouter(RoutingId.from("durable-native-caller"));
            port.ensureReceivePollerRegistered(caller);
            String endpoint = "inproc://durable-native-receiver-" + System.nanoTime();
            target.bind(endpoint);
            caller.connect(endpoint);
            var result =
                    ZLinkJavaDurableRequest.<Integer>request(
                                    () -> List.of(new byte[] {1}),
                                    (frames, remaining, admission, terminal) -> {
                                        attempts.incrementAndGet();
                                        var reply =
                                                port.requestTo(
                                                        caller,
                                                        targetId,
                                                        frames,
                                                        remaining,
                                                        parts -> {
                                                            decoded.incrementAndGet();
                                                            payload.set(parts.getFirst());
                                                            int value =
                                                                    parts.getFirst()
                                                                            .dataBuffer()
                                                                            .get(0);
                                                            if (codecThrows) throw codecFailure;
                                                            return value;
                                                        },
                                                        admission,
                                                        terminal);
                                        physical.set(reply);
                                        return reply;
                                    },
                                    () ->
                                            new systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkInternalMeshNode.PeerRouteObservation(
                                                    1, ready.get()),
                                    (changed, ended) -> {
                                        observations.incrementAndGet();
                                        ready.set(true);
                                        return () -> observerClosed.complete(null);
                                    },
                                    action -> {
                                        if (port.stateLane().isOnLane()) {
                                            action.run();
                                            return CompletableFuture.completedStage(null);
                                        }
                                        return port.stateLane()
                                                .runNowOrQueue(
                                                        () -> {
                                                            action.run();
                                                            return null;
                                                        });
                                    },
                                    Duration.ofSeconds(5),
                                    operations)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            Throwable cancellation = null;
            try (var incoming = port.receiveNow(target)) {
                assertNotNull(incoming);
                assertEquals(1, attempts.get());
                assertEquals(0, decoded.get());
                assertFalse(physical.get().toCompletableFuture().isDone());
                if (cancel) {
                    assertTrue(result.cancel(false));
                    cancellation = result.handle((reply, failure) -> failure).join();
                    assertInstanceOf(CancellationException.class, cancellation);
                    if (!initiallyReady) observerClosed.get(2, TimeUnit.SECONDS);
                    port.stateLane().awaitTurn(() -> null);
                    assertEquals(0, operations.pendingCount());
                    assertFalse(physical.get().toCompletableFuture().isDone());
                } else {
                    assertEquals(1, operations.pendingCount());
                    assertFalse(observerClosed.isDone());
                }
                port.reply(
                        target,
                        incoming.source(),
                        incoming.requestSequence(),
                        List.of(new byte[] {7}));
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            physical.get().toCompletableFuture().get(2, TimeUnit.SECONDS);
            port.stateLane().awaitTurn(() -> null);
            if (cancel) {
                assertSame(cancellation, result.handle((reply, failure) -> failure).join());
            } else if (codecThrows) {
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> result.get(2, TimeUnit.SECONDS));
                assertSame(codecFailure, failure.getCause());
            } else {
                assertEquals(7, result.get(2, TimeUnit.SECONDS));
            }
            if (!initiallyReady) observerClosed.get(2, TimeUnit.SECONDS);
            assertEquals(initiallyReady ? 0 : 1, observations.get());
            assertEquals(!initiallyReady, observerClosed.isDone());
            assertEquals(0, operations.pendingCount());
            assertEquals(1, attempts.get());
            assertEquals(1, decoded.get());
            assertFalse(physical.get().toCompletableFuture().isCancelled());
            assertNotNull(payload.get());
            assertConsumed(payload.get());
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false",
        "false,true",
        "true,false",
        "true,true"
    })
    void registryLogicalTerminalRetainsTheNativeReplyReceiver(boolean cancel, boolean codecThrows)
            throws Exception {
        var decoded = new AtomicInteger();
        var payload = new AtomicReference<Message>();
        var codecObserved = new CompletableFuture<Void>();
        var codecFailure = new IllegalStateException("controlled registry codec failure");
        try (var port = new ZLinkJavaRawServicePort()) {
            var targetId = RoutingId.from("registry-native-target");
            var target = port.openRouter(targetId);
            var caller = port.openRouter(RoutingId.from("registry-native-caller"));
            port.ensureReceivePollerRegistered(caller);
            String endpoint = "inproc://registry-native-receiver-" + System.nanoTime();
            target.bind(endpoint);
            caller.connect(endpoint);
            var request = Message.from(new byte[] {1});
            var result =
                    port.requestMessages(
                                    caller,
                                    targetId,
                                    List.of(request),
                                    Duration.ofSeconds(5),
                                    parts -> {
                                        decoded.incrementAndGet();
                                        payload.set(parts.getFirst());
                                        int value = parts.getFirst().dataBuffer().get(0);
                                        codecObserved.complete(null);
                                        if (codecThrows) throw codecFailure;
                                        return value;
                                    },
                                    ignored -> {},
                                    null,
                                    null)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            Throwable cancellation = null;
            try (var incoming = port.receiveNow(target)) {
                assertNotNull(incoming);
                assertEquals(0, decoded.get());
                assertEquals(1, port.operationRegistry().pendingCount());
                assertConsumed(request);
                if (cancel) {
                    assertTrue(result.cancel(false));
                    cancellation =
                            result.handle((reply, failure) -> failure).get(2, TimeUnit.SECONDS);
                    assertInstanceOf(CancellationException.class, cancellation);
                    port.stateLane().awaitTurn(() -> null);
                    assertTrue(result.isCancelled());
                    assertEquals(0, port.operationRegistry().pendingCount());
                }
                port.reply(
                        target,
                        incoming.source(),
                        incoming.requestSequence(),
                        List.of(new byte[] {7}));
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            codecObserved.get(2, TimeUnit.SECONDS);
            port.stateLane().awaitTurn(() -> null);
            if (cancel) {
                assertSame(cancellation, result.handle((reply, failure) -> failure).join());
                assertTrue(result.isCancelled());
            } else if (codecThrows) {
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> result.get(2, TimeUnit.SECONDS));
                assertSame(codecFailure, failure.getCause());
            } else {
                assertEquals(7, result.get(2, TimeUnit.SECONDS));
            }
            assertEquals(1, decoded.get());
            assertEquals(0, port.operationRegistry().pendingCount());
            assertNotNull(payload.get());
            assertConsumed(payload.get());
            assertNull(port.receiveNow(target), "the held native request must not be resubmitted");
        }
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "false,false",
        "false,true",
        "true,false",
        "true,true"
    })
    void startedLocalCanonicalProducerSurvivesLogicalCancellation(
            boolean cancel, boolean invalidReply) throws Exception {
        var physical = new CompletableFuture<byte[]>();
        var calls = new AtomicInteger();
        var local = RoutingId.from("registry-local-canonical");
        var port = new ZLinkJavaRawServicePort();
        try (var node =
                ZLinkJavaRawMeshNodeTesting.create(
                        "registry-local", port, null, null, null, Map.of(), Runnable::run)) {
            node.setRoutingId(local);
            node.setCanonicalRelocationControlHandler(
                    (source, sequence, command) -> {
                        calls.incrementAndGet();
                        return physical;
                    });
            var admitted = new CompletableFuture<Void>();
            var caller =
                    node.requestCanonicalRelocationPrepare(
                                    local, canonicalCommand(40), Duration.ofSeconds(5), admitted)
                            .toCompletableFuture();
            admitted.get(2, TimeUnit.SECONDS);
            assertEquals(1, calls.get());
            assertEquals(1, node.operationRegistry().pendingCount());
            assertFalse(physical.isDone());
            Throwable cancellation = null;
            if (cancel) {
                assertTrue(caller.cancel(false));
                cancellation = caller.handle((value, failure) -> failure).get(2, TimeUnit.SECONDS);
                assertInstanceOf(CancellationException.class, cancellation);
                assertEquals(0, node.operationRegistry().pendingCount());
                assertFalse(physical.isDone());
            }
            byte[] reply = invalidReply ? canonicalCommand(40) : canonicalCommand(30);
            assertTrue(physical.complete(reply));
            if (cancel) {
                assertSame(cancellation, caller.handle((value, failure) -> failure).join());
                assertTrue(caller.isCancelled());
            } else if (invalidReply) {
                var failure =
                        assertThrows(
                                ExecutionException.class, () -> caller.get(2, TimeUnit.SECONDS));
                assertInstanceOf(IllegalArgumentException.class, failure.getCause());
                assertEquals(
                        "canonical relocation prepare reply must be READY or FAILED",
                        failure.getCause().getMessage());
            } else {
                byte[] decoded = caller.get(2, TimeUnit.SECONDS);
                assertArrayEquals(reply, decoded);
                org.junit.jupiter.api.Assertions.assertNotSame(reply, decoded);
            }
            assertFalse(physical.isCancelled());
            assertEquals(1, calls.get());
            assertEquals(0, node.operationRegistry().pendingCount());
        }
    }

    private static byte[] canonicalCommand(int command) throws Exception {
        for (java.nio.file.Path current = java.nio.file.Path.of("").toAbsolutePath();
                current != null;
                current = current.getParent()) {
            for (String relative :
                    List.of(
                            "framework/runtime/protocol/golden/relocation-control-v1.json",
                            "runtime/protocol/golden/relocation-control-v1.json")) {
                var file = current.resolve(relative);
                if (!java.nio.file.Files.isRegularFile(file)) continue;
                for (var entry :
                        new com.fasterxml.jackson.databind.ObjectMapper()
                                .readTree(java.nio.file.Files.readString(file))
                                .path("canonical")) {
                    if (entry.path("command").asInt() == command)
                        return HexFormat.of().parseHex(entry.path("hex").asText());
                }
            }
        }
        throw new IllegalStateException("canonical relocation command was not found: " + command);
    }

    @Test
    void nativeReplyPreservesWireBytesAndConsumesMessagesOnEveryTerminalPath() throws Exception {
        RoutingId callerId = RoutingId.from("native-reply-owner-caller");
        RoutingId targetId = RoutingId.from("native-reply-owner-target");
        byte[] expected = new byte[4096];
        for (int index = 0; index < expected.length; index++) {
            expected[index] = (byte) (index * 31 + 7);
        }
        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
                ZLinkJavaRawServicePort foreign = new ZLinkJavaRawServicePort()) {
            var target = port.openRouter(targetId);
            var caller = port.openRouter(callerId);
            port.ensureReceivePollerRegistered(caller);
            var other = foreign.openRouter(RoutingId.from("native-reply-foreign"));
            String endpoint = "inproc://native-reply-ownership-" + System.nanoTime();
            target.bind(endpoint);
            caller.connect(endpoint);
            var reply =
                    port.request(
                                    caller,
                                    targetId,
                                    List.of(new byte[] {1}),
                                    Duration.ofSeconds(2),
                                    null)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            ReplyToken lateToken;
            try (var incoming = port.receiveNow(target)) {
                assertNotNull(incoming);
                lateToken = incoming.requestSequence();
                Message rejected = Message.from(expected);
                assertThrows(
                        IllegalArgumentException.class,
                        () ->
                                port.replyMessages(
                                        other,
                                        incoming.source(),
                                        incoming.requestSequence(),
                                        List.of(rejected)));
                assertConsumed(rejected);

                Message invalidToken = Message.from(expected);
                assertThrows(
                        IllegalArgumentException.class,
                        () ->
                                port.replyMessages(
                                        target, incoming.source(), null, List.of(invalidToken)));
                assertConsumed(invalidToken);

                Message header = Message.from(new byte[] {0, 127, -1});
                Message payload = Message.from(expected);
                port.replyMessages(
                        target,
                        incoming.source(),
                        incoming.requestSequence(),
                        List.of(header, payload));
                assertConsumed(header);
                assertConsumed(payload);
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            List<byte[]> received = reply.get(2, TimeUnit.SECONDS);
            assertEquals(2, received.size());
            assertArrayEquals(new byte[] {0, 127, -1}, received.getFirst());
            assertArrayEquals(expected, received.get(1));

            port.close();
            Message closed = Message.from(expected);
            IllegalStateException closedFailure =
                    assertThrows(
                            IllegalStateException.class,
                            () -> port.replyMessages(target, callerId, lateToken, List.of(closed)));
            assertEquals("service port is closed", closedFailure.getMessage());
            assertConsumed(closed);
        }
    }

    @Test
    void concurrentRepliesFromDifferentThreadsKeepTheirRequestTokens() throws Exception {
        RoutingId targetId = RoutingId.from("parallel-reply-target");
        String endpoint = "inproc://parallel-replies-" + System.nanoTime();
        int count = 8;

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
                var workers = Executors.newFixedThreadPool(count)) {
            var target = port.openRouter(targetId);
            target.bind(endpoint);

            List<RouterSocket> callers = new ArrayList<>(count);
            List<CompletableFuture<List<byte[]>>> replies = new ArrayList<>(count);
            for (int index = 0; index < count; index++) {
                var caller = port.openRouter(RoutingId.from("parallel-reply-caller-" + index));
                port.ensureReceivePollerRegistered(caller);
                caller.connect(endpoint);
                callers.add(caller);
                replies.add(
                        port.request(
                                        caller,
                                        targetId,
                                        List.of(new byte[] {(byte) index}),
                                        Duration.ofSeconds(3),
                                        null)
                                .toCompletableFuture());
            }

            List<Future<?>> submissions = new ArrayList<>(count);
            for (int index = 0; index < count; index++) {
                assertTrue(port.waitForReadable(target, Duration.ofSeconds(3)));
                var inbound = port.receiveNow(target);
                assertNotNull(inbound);
                int requestId = Byte.toUnsignedInt(inbound.frames().getFirst()[0]);
                RoutingId source = inbound.source();
                var token = inbound.requestSequence();
                submissions.add(
                        workers.submit(
                                () -> {
                                    try {
                                        port.reply(
                                                target,
                                                source,
                                                token,
                                                List.of(new byte[] {(byte) (requestId + 64)}));
                                    } finally {
                                        inbound.close();
                                    }
                                }));
            }

            for (var submission : submissions) submission.get(3, TimeUnit.SECONDS);
            for (var caller : callers) {
                port.waitForReadable(caller, Duration.ofSeconds(3));
            }
            for (int index = 0; index < count; index++) {
                List<byte[]> result = replies.get(index).get(3, TimeUnit.SECONDS);
                assertEquals(1, result.size());
                assertEquals(index + 64, Byte.toUnsignedInt(result.getFirst()[0]));
            }
        }
    }

    @RepeatedTest(100)
    void repliesRacingRuntimeCloseAreRejectedAfterCloseStartsAndCloseSucceeds() throws Exception {
        Context context = Zlink.createContext();
        var runtime = new ZLinkJavaRawMeshNode(context, "reply-close-race");
        try {
            assertReplyRacingClose(runtime.port, runtime::close, "runtime");
        } finally {
            runtime.close();
            context.close();
        }
    }

    @RepeatedTest(100)
    void repliesRacingPortCloseAreRejectedAfterCloseStartsAndCloseSucceeds() throws Exception {
        var port = new ZLinkJavaRawServicePort();
        try {
            assertReplyRacingClose(port, port::close, "port");
        } finally {
            port.close();
        }
    }

    @Test
    void replyAlreadySubmittingLeavesAfterOneBusyAndCloseRetrySucceeds() throws Exception {
        var submitEntered = new CountDownLatch(1);
        var allowSubmitToLeave = new CountDownLatch(1);
        var submitLeft = new CountDownLatch(1);
        var closeSawBusy = new CountDownLatch(1);
        var closeAttempts = new AtomicInteger();
        var busyAttempts = new AtomicInteger();
        var busy = new ZlinkCloseException(CloseResult.BUSY, 16);

        ReplySubmitOperation submitOperation =
                (ReplySubmitOperation)
                        java.lang.reflect.Proxy.newProxyInstance(
                                ReplySubmitOperation.class.getClassLoader(),
                                new Class<?>[] {ReplySubmitOperation.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("message")) return proxy;
                                    if (method.getName().equals("submit")) {
                                        submitEntered.countDown();
                                        try {
                                            assertTrue(
                                                    allowSubmitToLeave.await(2, TimeUnit.SECONDS));
                                        } finally {
                                            submitLeft.countDown();
                                        }
                                        return null;
                                    }
                                    throw new AssertionError(method.getName());
                                });
        ReplyOperation replyOperation =
                (ReplyOperation)
                        java.lang.reflect.Proxy.newProxyInstance(
                                ReplyOperation.class.getClassLoader(),
                                new Class<?>[] {ReplyOperation.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("message")) return submitOperation;
                                    throw new AssertionError(method.getName());
                                });
        RouterSocket router =
                (RouterSocket)
                        java.lang.reflect.Proxy.newProxyInstance(
                                RouterSocket.class.getClassLoader(),
                                new Class<?>[] {RouterSocket.class},
                                (proxy, method, args) -> {
                                    switch (method.getName()) {
                                        case "hashCode":
                                            return System.identityHashCode(proxy);
                                        case "equals":
                                            return proxy == args[0];
                                        case "setRoutingId":
                                            return null;
                                        case "reply":
                                            return replyOperation;
                                        case "close":
                                            if (closeAttempts.incrementAndGet() == 1) {
                                                busyAttempts.incrementAndGet();
                                                closeSawBusy.countDown();
                                                throw busy;
                                            }
                                            assertTrue(submitLeft.await(2, TimeUnit.SECONDS));
                                            return null;
                                        default:
                                            throw new AssertionError(method.getName());
                                    }
                                });
        Context context =
                (Context)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Context.class.getClassLoader(),
                                new Class<?>[] {Context.class},
                                (proxy, method, args) -> {
                                    assertEquals("createRouterSocket", method.getName());
                                    return router;
                                });
        Poller poller =
                (Poller)
                        java.lang.reflect.Proxy.newProxyInstance(
                                Poller.class.getClassLoader(),
                                new Class<?>[] {Poller.class},
                                (proxy, method, args) -> null);
        var port =
                new ZLinkJavaRawServicePort(
                        context,
                        new systems.zlink.framework.runtime.internal.execution.ZLinkStateLane(),
                        (socket, observesRoutes) ->
                                new ZLinkJavaSocketReceivePoller(
                                        socket, true, observesRoutes, () -> poller, () -> null));
        var workers = Executors.newFixedThreadPool(2);
        Message message = Message.from(new byte[] {1});
        try {
            port.openRouter(RoutingId.from("ordered-close"));
            var reply =
                    workers.submit(
                            () ->
                                    port.replyMessages(
                                            router,
                                            RoutingId.from("caller"),
                                            systems.zlink.contracts.messaging.ReceivedTesting
                                                    .replyToken(router, 1L),
                                            List.of(message)));
            assertTrue(submitEntered.await(2, TimeUnit.SECONDS));
            var close = workers.submit(port::close);
            assertTrue(closeSawBusy.await(2, TimeUnit.SECONDS));
            allowSubmitToLeave.countDown();
            reply.get(2, TimeUnit.SECONDS);
            close.get(2, TimeUnit.SECONDS);
            assertEquals(1, busyAttempts.get());
            assertEquals(2, closeAttempts.get());
            assertConsumed(message);
        } finally {
            allowSubmitToLeave.countDown();
            workers.shutdownNow();
            port.close();
        }
    }

    private static void assertReplyRacingClose(
            ZLinkJavaRawServicePort port, Runnable close, String closeName) throws Exception {
        int count = 8;
        int threadCount = 4;
        RoutingId targetId = RoutingId.from("reply-close-target-" + closeName);
        String endpoint = "inproc://reply-close-race-" + closeName + "-" + System.nanoTime();
        var target = port.openRouter(targetId);
        target.bind(endpoint);
        List<CompletableFuture<List<byte[]>>> requests = new ArrayList<>(count);
        List<ZLinkJavaRawServicePort.Inbound> inbounds = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            var caller = port.openRouter(RoutingId.from("reply-close-caller-" + closeName + index));
            port.ensureReceivePollerRegistered(caller);
            caller.connect(endpoint);
            requests.add(
                    port.request(
                                    caller,
                                    targetId,
                                    List.of(new byte[] {(byte) index}),
                                    Duration.ofSeconds(5),
                                    null)
                            .toCompletableFuture());
        }
        for (int index = 0; index < count; index++) {
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(3)));
            var inbound = port.receiveNow(target);
            assertNotNull(inbound);
            inbounds.add(inbound);
        }

        var start = new CountDownLatch(1);
        List<CompletableFuture<Throwable>> replyResults = new ArrayList<>(count);
        List<Message> responses = new ArrayList<>(count);
        for (int index = 0; index < count; index++) {
            replyResults.add(new CompletableFuture<>());
            responses.add(Message.from(new byte[] {(byte) (index + 32)}));
        }
        var workers = Executors.newFixedThreadPool(threadCount);
        List<Future<?>> replyThreads = new ArrayList<>(threadCount);
        for (int workerIndex = 0; workerIndex < threadCount; workerIndex++) {
            final int firstReplyIndex = workerIndex;
            replyThreads.add(
                    workers.submit(
                            () -> {
                                try {
                                    start.await();
                                    for (int replyIndex = firstReplyIndex;
                                            replyIndex < count;
                                            replyIndex += threadCount) {
                                        try {
                                            var inbound = inbounds.get(replyIndex);
                                            port.replyMessages(
                                                    target,
                                                    inbound.source(),
                                                    inbound.requestSequence(),
                                                    List.of(responses.get(replyIndex)));
                                            replyResults.get(replyIndex).complete(null);
                                        } catch (Throwable failure) {
                                            replyResults.get(replyIndex).complete(failure);
                                        }
                                    }
                                } catch (InterruptedException interrupted) {
                                    Thread.currentThread().interrupt();
                                    for (int replyIndex = firstReplyIndex;
                                            replyIndex < count;
                                            replyIndex += threadCount) {
                                        replyResults.get(replyIndex).complete(interrupted);
                                    }
                                }
                            }));
        }
        var closeResult = new CompletableFuture<Void>();
        Thread closeThread =
                Thread.ofPlatform()
                        .daemon()
                        .start(
                                () -> {
                                    try {
                                        start.await();
                                        close.run();
                                        closeResult.complete(null);
                                    } catch (Throwable failure) {
                                        closeResult.completeExceptionally(failure);
                                    }
                                });
        start.countDown();
        try {
            closeResult.get(3, TimeUnit.SECONDS);
            for (var replyThread : replyThreads) replyThread.get(3, TimeUnit.SECONDS);
            workers.shutdown();
            assertTrue(workers.awaitTermination(3, TimeUnit.SECONDS));
            closeThread.join(3000);
            assertFalse(closeThread.isAlive());
            for (int index = 0; index < count; index++) {
                Throwable result = replyResults.get(index).get(3, TimeUnit.SECONDS);
                // A reply that passed the closed check before close started can reach the
                // binding after its socket closed; the binding then rejects it as closed.
                assertTrue(
                        result == null
                                || (result instanceof IllegalStateException
                                        && ("service port is closed".equals(result.getMessage())
                                                || "socket is closed".equals(result.getMessage())))
                                || (result instanceof ZlinkSubmitException submitFailure
                                        && submitFailure.getResult()
                                                == systems.zlink.contracts.sockets.SubmitResult
                                                        .TERMINATED),
                        String.valueOf(result));
                assertConsumed(responses.get(index));
                try {
                    requests.get(index).get(3, TimeUnit.SECONDS);
                } catch (ExecutionException expected) {
                    // Closing either socket or rejecting the late reply ends the request as
                    // failure.
                }
            }
            Message afterClose = Message.from(new byte[] {99});
            IllegalStateException rejected =
                    assertThrows(
                            IllegalStateException.class,
                            () ->
                                    port.replyMessages(
                                            target,
                                            inbounds.getFirst().source(),
                                            inbounds.getFirst().requestSequence(),
                                            List.of(afterClose)));
            assertEquals("service port is closed", rejected.getMessage());
            assertConsumed(afterClose);
        } finally {
            workers.shutdownNow();
            for (var inbound : inbounds) inbound.close();
        }
    }

    @Test
    void nativeSendConsumesMessagesOnSuccessAndSocketRejection() throws Exception {
        RoutingId receiverId = RoutingId.from("native-send-receiver");
        RoutingId senderId = RoutingId.from("native-send-sender");
        String endpoint = "inproc://native-send-ownership-" + System.nanoTime();

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
                ZLinkJavaRawServicePort foreign = new ZLinkJavaRawServicePort()) {
            var receiver = port.openRouter(receiverId);
            var sender = port.openRouter(senderId);
            var foreignRouter = foreign.openRouter(RoutingId.from("native-send-foreign"));
            receiver.bind(endpoint);
            sender.connect(endpoint);

            Message sent = Message.from(new byte[] {1, 2, 3});
            port.sendMessages(sender, receiverId, List.of(sent))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertConsumed(sent);
            assertTrue(port.waitForReadable(receiver, Duration.ofSeconds(2)));
            try (var inbound = port.receiveNow(receiver)) {
                assertNotNull(inbound);
                assertArrayEquals(new byte[] {1, 2, 3}, inbound.frames().getFirst());
            }

            Message rejected = Message.from(new byte[] {4, 5, 6});
            assertThrows(
                    IllegalArgumentException.class,
                    () -> port.sendMessages(foreignRouter, receiverId, List.of(rejected)));
            assertConsumed(rejected);

            Message socketRejected = Message.from(new byte[] {7, 8, 9});
            var failed =
                    port.sendMessages(
                                    sender,
                                    RoutingId.from("native-send-missing"),
                                    List.of(socketRejected))
                            .toCompletableFuture();
            assertThrows(ExecutionException.class, () -> failed.get(2, TimeUnit.SECONDS));
            assertConsumed(socketRejected);
        }
    }

    @Test
    void nativeRequestConsumesMessagesOnSuccessAndAsyncFailure() throws Exception {
        RoutingId targetId = RoutingId.from("native-request-target");
        RoutingId callerId = RoutingId.from("native-request-caller");
        String endpoint = "inproc://native-request-ownership-" + System.nanoTime();

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort()) {
            var target = port.openRouter(targetId);
            var caller = port.openRouter(callerId);
            port.ensureReceivePollerRegistered(caller);
            target.bind(endpoint);
            caller.connect(endpoint);

            Message successful = Message.from(new byte[] {7});
            var reply =
                    port.requestMessages(
                                    caller,
                                    targetId,
                                    List.of(successful),
                                    Duration.ofSeconds(2),
                                    parts -> parts.getFirst().readByte(0),
                                    ignored -> {},
                                    null,
                                    null)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var inbound = port.receiveNow(target)) {
                assertNotNull(inbound);
                port.reply(
                        target,
                        inbound.source(),
                        inbound.requestSequence(),
                        List.of(new byte[] {9}));
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            assertEquals(9, Byte.toUnsignedInt(reply.get(2, TimeUnit.SECONDS)));
            assertConsumed(successful);

            Message timedOut = Message.from(new byte[] {8});
            var failure =
                    port.requestMessages(
                                    caller,
                                    targetId,
                                    List.of(timedOut),
                                    Duration.ofMillis(25),
                                    parts -> parts.getFirst().readByte(0),
                                    ignored -> {},
                                    null,
                                    null)
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var ignored = port.receiveNow(target)) {
                assertNotNull(ignored);
                // Keep the request unanswered so its accepted async path times out.
            }
            port.waitForReadable(caller, Duration.ofSeconds(2));
            assertThrows(ExecutionException.class, () -> failure.get(2, TimeUnit.SECONDS));
            assertConsumed(timedOut);

            Message routeRejected = Message.from(new byte[] {10});
            var unavailable =
                    port.requestMessages(
                                    caller,
                                    RoutingId.from("native-request-missing"),
                                    List.of(routeRejected),
                                    Duration.ofSeconds(2),
                                    parts -> parts.getFirst().readByte(0),
                                    ignored -> {},
                                    null,
                                    null)
                            .toCompletableFuture();
            var rejected =
                    assertThrows(
                            ExecutionException.class, () -> unavailable.get(2, TimeUnit.SECONDS));
            var framework = assertInstanceOf(ZLinkFrameworkException.class, rejected.getCause());
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, framework.kind());
            assertConsumed(routeRejected);
        }
    }

    @Test
    void blockingReadinessSleepsUntilArrivalAndWakesImmediately() throws Exception {
        RoutingId leftRid = RoutingId.from("readiness-left");
        RoutingId rightRid = RoutingId.from("readiness-right");
        String endpoint = "inproc://readiness-" + System.nanoTime();

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
                ExecutorService executor = Executors.newSingleThreadExecutor()) {
            var left = port.openRouter(leftRid);
            var right = port.openRouter(rightRid);
            left.bind(endpoint);
            right.connect(endpoint);

            var readiness =
                    executor.submit(() -> port.waitForReadable(left, Duration.ofSeconds(2)));
            Thread.sleep(50);
            assertFalse(readiness.isDone());
            long sentAt = System.nanoTime();
            port.send(right, leftRid, List.of(new byte[] {1}))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);

            assertTrue(readiness.get(2, TimeUnit.SECONDS));
            assertTrue(
                    Duration.ofNanos(System.nanoTime() - sentAt).compareTo(Duration.ofMillis(250))
                            < 0);
            try (var inbound = port.receiveNow(left)) {
                assertNotNull(inbound);
                assertArrayEquals(new byte[] {1}, inbound.frames().getFirst());
            }
        }
    }

    @Test
    void closesOwnedRawResourcesOnceAndRejectsNewResourcesAfterClose() {
        ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
        port.openRouter(RoutingId.from("m5-resource-owner"));

        port.close();

        assertDoesNotThrow(port::close);
        assertThrows(
                IllegalStateException.class, () -> port.openRouter(RoutingId.from("after-close")));
    }

    @RepeatedTest(20)
    void closeDrainsStartedSubmissionsBeforeClosingTheirSocket() throws Exception {
        RoutingId receiver = RoutingId.from("close-receiver");
        try (var port = new ZLinkJavaRawServicePort();
                var executor = Executors.newSingleThreadExecutor()) {
            var left = port.openRouter(receiver);
            var right = port.openRouter(RoutingId.from("close-sender"));
            String endpoint = "inproc://close-submission-" + System.nanoTime();
            left.bind(endpoint);
            right.connect(endpoint);
            port.send(right, receiver, List.of(new byte[] {0}))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            awaitInbound(port, left, 1, System.nanoTime() + Duration.ofSeconds(2).toNanos());

            var submitting = new CountDownLatch(1);
            var sends =
                    executor.submit(
                            () -> {
                                for (int index = 0; index < 16; index++) {
                                    submitting.countDown();
                                    try {
                                        port.send(right, receiver, List.of(new byte[] {1}))
                                                .toCompletableFuture()
                                                .join();
                                    } catch (IllegalStateException closed) {
                                        assertEquals("service port is closed", closed.getMessage());
                                        return;
                                    }
                                }
                            });
            assertTrue(submitting.await(2, TimeUnit.SECONDS));
            assertDoesNotThrow(port::close);
            sends.get(2, TimeUnit.SECONDS);
        }
    }

    @Test
    void inboundRetainsOneMultipartOwnerWithSourceRoutingId() throws Exception {
        RoutingId leftRid = RoutingId.from("m5-left");
        RoutingId rightRid = RoutingId.from("m5-right");
        String endpoint = "inproc://m5-raw-service-port-" + System.nanoTime();
        byte[] first = new byte[] {1, 2, 3};
        byte[] second = new byte[] {4, 5, 6};

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort()) {
            var left = port.openRouter(leftRid);
            var right = port.openRouter(rightRid);
            left.bind(endpoint);
            right.connect(endpoint);

            long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            port.send(right, leftRid, List.of(first, second))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            first[0] = 9;
            second[0] = 9;

            ZLinkJavaRawServicePort.Inbound received = null;
            while (received == null) {
                received = port.receive(left);
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError("raw multipart receive timed out");
                }
                if (received == null) {
                    Thread.sleep(1);
                }
            }

            assertEquals(rightRid, received.source());
            try (var inbound = received) {
                List<byte[]> retained = inbound.frames();
                assertArrayEquals(new byte[] {1, 2, 3}, retained.get(0));
                assertArrayEquals(new byte[] {4, 5, 6}, retained.get(1));
                assertSame(retained, inbound.frames());
            }
        }
    }

    @Test
    void serializesConcurrentMultipartSubmitsOnOneRouter() throws Exception {
        RoutingId leftRid = RoutingId.from("m5-concurrent-left");
        RoutingId rightRid = RoutingId.from("m5-concurrent-right");
        String endpoint = "inproc://m5-raw-service-port-concurrent-" + System.nanoTime();

        try (ZLinkJavaRawServicePort port = new ZLinkJavaRawServicePort();
                ExecutorService executor = Executors.newFixedThreadPool(2)) {
            var left = port.openRouter(leftRid);
            var right = port.openRouter(rightRid);
            left.bind(endpoint);
            right.connect(endpoint);

            long connectDeadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
            port.send(right, leftRid, List.of(new byte[] {0}))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            awaitInbound(port, left, 1, connectDeadline);

            CountDownLatch start = new CountDownLatch(1);
            var first = executor.submit(() -> sendMany(port, right, leftRid, (byte) 1, start));
            var second = executor.submit(() -> sendMany(port, right, leftRid, (byte) 2, start));
            start.countDown();

            first.get(5, TimeUnit.SECONDS);
            second.get(5, TimeUnit.SECONDS);
            awaitInbound(port, left, 32, System.nanoTime() + Duration.ofSeconds(2).toNanos());
        }
    }

    private static void sendMany(
            ZLinkJavaRawServicePort port,
            RouterSocket router,
            RoutingId target,
            byte marker,
            CountDownLatch start) {
        try {
            start.await();
            for (int index = 0; index < 16; index++) {
                port.send(router, target, List.of(new byte[] {marker, (byte) index}))
                        .toCompletableFuture()
                        .get(2, TimeUnit.SECONDS);
            }
        } catch (InterruptedException interrupted) {
            Thread.currentThread().interrupt();
            throw new AssertionError("concurrent raw multipart send interrupted", interrupted);
        } catch (Exception failure) {
            throw new AssertionError("concurrent raw multipart send failed", failure);
        }
    }

    private static void assertConsumed(Message message) {
        assertThrows(IllegalStateException.class, message::refCount);
    }

    private static void awaitInbound(
            ZLinkJavaRawServicePort port, RouterSocket router, int expected, long deadline)
            throws Exception {
        int receivedCount = 0;
        while (receivedCount < expected) {
            try (var inbound = port.receive(router)) {
                if (inbound != null) {
                    receivedCount++;
                    continue;
                }
            }
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("raw concurrent multipart receive timed out");
            }
            Thread.sleep(1);
        }
    }
}
