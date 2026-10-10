package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaRawServicePortContractTest {
    @Test
    void tokenlessRequestCapacityFailureRetainsItsKindAndCauseThroughMeshCompletion()
            throws Exception {
        var refusal =
                new systems.zlink.contracts.errors.ZlinkSubmitException(
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
                            ignored -> {});
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
            var mapper =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "mapRequestFailure",
                            java.util.concurrent.CompletionStage.class,
                            java.util.function.Function.class);
            mapper.setAccessible(true);
            java.util.function.Function<
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
            var mapped =
                    (java.util.concurrent.CompletionStage<?>)
                            mapper.invoke(null, submitted, decodeFailure);
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
        var attempts = new java.util.concurrent.atomic.AtomicInteger();
        var failure =
                new systems.zlink.contracts.errors.ZlinkCloseException(
                        systems.zlink.contracts.errors.CloseResult.BUSY, 16);
        try (var context = systems.zlink.contracts.core.Zlink.createContext();
                var router = context.createRouterSocket()) {
            var owner = new ZLinkJavaSocketReceivePoller(router);
            var field = ZLinkJavaSocketReceivePoller.class.getDeclaredField("poller");
            field.setAccessible(true);
            ((systems.zlink.contracts.eventing.Poller) field.get(owner)).close();
            field.set(
                    owner,
                    java.lang.reflect.Proxy.newProxyInstance(
                            systems.zlink.contracts.eventing.Poller.class.getClassLoader(),
                            new Class<?>[] {systems.zlink.contracts.eventing.Poller.class},
                            (proxy, method, args) -> {
                                assertEquals("close", method.getName());
                                if (attempts.incrementAndGet() == 1) {
                                    throw failure;
                                }
                                return null;
                            }));
            assertSame(failure, assertThrows(failure.getClass(), owner::close));
            owner.close();
            owner.close();
            assertEquals(2, attempts.get());
        }
    }

    @Test
    void failedSocketCloseRetainsOwnershipUntilLaterCleanupSucceeds() throws Exception {
        var attempts = new java.util.concurrent.atomic.AtomicInteger();
        var failure =
                new systems.zlink.contracts.errors.ZlinkCloseException(
                        systems.zlink.contracts.errors.CloseResult.BUSY, 16);
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
                                            if (attempts.incrementAndGet() == 1) {
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
            assertEquals(2, attempts.get());
            port.close();
            assertEquals(2, attempts.get());
        }
    }

    @Test
    void oneOrderedRegistryOwnsSocketsAndRejectsForeignSockets() throws Exception {
        try (ZLinkJavaRawServicePort owner = new ZLinkJavaRawServicePort();
                ZLinkJavaRawServicePort foreign = new ZLinkJavaRawServicePort()) {
            var first = owner.openRouter(RoutingId.from("owner-first"));
            var second = owner.openRouter(RoutingId.from("owner-second"));
            var other = foreign.openRouter(RoutingId.from("owner-first"));
            var registryField = ZLinkJavaRawServicePort.class.getDeclaredField("receivePollers");
            registryField.setAccessible(true);
            var registry = (java.util.SequencedMap<?, ?>) registryField.get(owner);
            assertEquals(List.of(second, first), List.copyOf(registry.reversed().keySet()));
            assertFalse(
                    java.util.Arrays.stream(ZLinkJavaRawServicePort.class.getDeclaredFields())
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
                                    })
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var incoming = port.receiveNow(target).orElseThrow()) {
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
                    port.request(caller, targetId, List.of(new byte[] {1}), Duration.ofSeconds(2))
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var incoming = port.receiveNow(target).orElseThrow()) {
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
            assertThrows(
                    IllegalStateException.class,
                    () -> port.replyMessages(target, callerId, null, List.of(closed)));
            assertConsumed(closed);
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
            try (var inbound = port.receiveNow(receiver).orElseThrow()) {
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
                                    ignored -> {})
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var inbound = port.receiveNow(target).orElseThrow()) {
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
                                    ignored -> {})
                            .toCompletableFuture();
            assertTrue(port.waitForReadable(target, Duration.ofSeconds(2)));
            try (var ignored = port.receiveNow(target).orElseThrow()) {
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
                                    ignored -> {})
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
            try (var inbound = port.receiveNow(left).orElseThrow()) {
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

            Optional<ZLinkJavaRawServicePort.Inbound> received = Optional.empty();
            while (received.isEmpty()) {
                received = port.receive(left);
                if (System.nanoTime() >= deadline) {
                    throw new AssertionError("raw multipart receive timed out");
                }
                if (received.isEmpty()) {
                    Thread.sleep(1);
                }
            }

            assertEquals(rightRid, received.orElseThrow().source());
            try (var inbound = received.orElseThrow()) {
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
            if (port.receive(router).isPresent()) {
                receivedCount++;
                continue;
            }
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("raw concurrent multipart receive timed out");
            }
            Thread.sleep(1);
        }
    }
}
