package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ConnectorDispatchTest {
    @Test
    void dispatchModeSurfaceUsesContractNames() {
        assertEquals(
                List.of("MANUAL", "IMMEDIATE"),
                Arrays.stream(ZLinkStreamDispatchMode.values()).map(Enum::name).toList());
    }

    @Test
    void manualDispatchStartsNextCallbackWithoutWaitingForPreviousCompletion() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                CompletableFuture<Void> callback = new CompletableFuture<>();
                AtomicInteger invoked = new AtomicInteger();
                connector.on(
                        "Slow",
                        message -> {
                            invoked.incrementAndGet();
                            message.payload().payload().close();
                            return callback;
                        });
                connector.on(
                        "Slow",
                        message -> {
                            invoked.incrementAndGet();
                            message.payload().payload().close();
                            return CompletableFuture.completedFuture(null);
                        });
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        0,
                                        null,
                                        "Slow",
                                        Map.of(),
                                        null),
                                TcpStreamConnectorTestServer.bytes("body"))
                        .join();
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.receivedCount("Slow") == 1);

                CompletableFuture<Void> dispatched =
                        connector.dispatch().submit().toCompletableFuture();
                assertTrue(dispatched.isDone());
                assertEquals(2, invoked.get());
                callback.complete(null);
                dispatched.get();
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void manualLifecycleCallbacksRunOnlyDuringDispatch() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            List<ZLinkStreamConnectionState> states = new ArrayList<>();
            AtomicInteger disconnected = new AtomicInteger();
            connector.onConnectionStateChanged(
                    state -> {
                        states.add(state);
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onDisconnected(
                    event -> {
                        disconnected.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            assertEquals(List.of(), states);
            ConnectorTestAwait.await(connector.dispatch());
            assertEquals(
                    List.of(
                            ZLinkStreamConnectionState.CONNECTING,
                            ZLinkStreamConnectionState.CONNECTED),
                    states);

            ConnectorTestAwait.await(connector.close());
            assertEquals(0, disconnected.get());
            ConnectorTestAwait.await(connector.dispatch());
            assertEquals(ZLinkStreamConnectionState.CLOSED, states.get(states.size() - 1));
            assertEquals(1, disconnected.get());
        }
    }

    @Test
    void closePreservesCallbacksAcceptedBeforeTheTerminalNotifications() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            List<ZLinkStreamConnectionState> states = new ArrayList<>();
            connector.onConnectionStateChanged(
                    state -> {
                        states.add(state);
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            ConnectorTestAwait.await(connector.close());
            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(
                    List.of(
                            ZLinkStreamConnectionState.CONNECTING,
                            ZLinkStreamConnectionState.CONNECTED,
                            ZLinkStreamConnectionState.CLOSED),
                    states);
        }
    }

    /**
     * Spec 32 7: a close called inside a callback returns after starting the close work, and the
     * close called outside waits for that same result.
     */
    @Test
    void closeInsideACloseCallbackReturnsTheSharedPendingCloseResult() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            List<CompletableFuture<Void>> inner = new ArrayList<>();
            List<Boolean> innerDoneOnReturn = new ArrayList<>();
            AtomicInteger disconnectedBeforeInnerReturn = new AtomicInteger(-1);
            AtomicInteger disconnected = new AtomicInteger();
            connector.onConnectionStateChanged(
                    state -> {
                        if (state == ZLinkStreamConnectionState.CLOSED) {
                            CompletableFuture<Void> closing =
                                    connector.close().submit().toCompletableFuture();
                            inner.add(closing);
                            innerDoneOnReturn.add(closing.isDone());
                            disconnectedBeforeInnerReturn.set(disconnected.get());
                        }
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onDisconnected(
                    event -> {
                        disconnected.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    });
            ConnectorTestAwait.await(connector.connect());

            CompletableFuture<Void> outer = connector.close().submit().toCompletableFuture();
            outer.get(5, TimeUnit.SECONDS);

            assertEquals(List.of(false), innerDoneOnReturn);
            assertEquals(0, disconnectedBeforeInnerReturn.get());
            assertEquals(1, disconnected.get());
            inner.get(0).get(5, TimeUnit.SECONDS);
        }
    }

    /**
     * Spec 32 10: undispatched received messages are cleared only when a connection is established,
     * so close keeps them for the next pump.
     */
    @Test
    void closeKeepsUndispatchedReceivedMessages() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            List<String> handled = new ArrayList<>();
            connector.on(
                    "Kept",
                    message -> {
                        handled.add(
                                new String(
                                        message.payload().payload().toByteArray(),
                                        StandardCharsets.UTF_8));
                        message.payload().payload().close();
                        return CompletableFuture.completedFuture(null);
                    });
            ConnectorTestAwait.await(connector.connect());
            ConnectorTestAwait.await(connector.dispatch());
            server.sendAsync(
                            new ZLinkStreamWireProtocol.Header(
                                    ZLinkStreamWireProtocol.KIND_SEND,
                                    ZLinkStreamWireProtocol.CODEC_RAW,
                                    0,
                                    null,
                                    "Kept",
                                    Map.of(),
                                    null),
                            TcpStreamConnectorTestServer.bytes("kept"))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);

            ConnectorTestAwait.await(connector.close());
            assertEquals(1, connector.receivedCount("Kept"));
            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(List.of("kept"), handled);
        }
    }

    @Test
    void dispatch_invokesCallback() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                AtomicInteger handled = new AtomicInteger();
                connector.on(
                        "Ping",
                        message -> {
                            handled.incrementAndGet();
                            assertEquals("Ping", message.packetName());
                            assertEquals("42", message.metadata().get("seq"));
                            assertEquals(
                                    "hello",
                                    new String(
                                            message.payload().payload().toByteArray(),
                                            StandardCharsets.UTF_8));
                            message.payload().payload().close();
                            return CompletableFuture.completedFuture(null);
                        });

                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
                                        null,
                                        "Ping",
                                        Map.of("seq", "42"),
                                        null),
                                TcpStreamConnectorTestServer.bytes("hello"))
                        .join();

                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.pendingDispatchCount() == 1);
                assertEquals(1, connector.pendingDispatchCount());
                assertEquals(0, handled.get());

                ConnectorTestAwait.await(connector.dispatch());

                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.pendingDispatchCount() == 0 && handled.get() == 1);
                assertEquals(1, handled.get());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void cancelledQueuedWaiterClosesTheMessageItCannotReceive() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> queued = message("queued");
        queue.addMessage(
                queued, () -> () -> CompletableFuture.completedFuture(null), () -> 1, false);

        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> cancelled =
                new CompletableFuture<>();
        assertTrue(cancelled.cancel(false));

        queue.awaitMessage("Push", ignored -> true, cancelled);

        assertEquals(0, queued.payload().payload().size());
        //  The message did arrive, so spec 32 10 keeps it counted even
        //  though the cancelled waiter could not take it.
        assertEquals(1, queue.receivedCount("Push"));
    }

    /**
     * Spec 32 7: a {@code Manual} queue grows until the user pumps, so the drain has to survive a
     * queue longer than the stack. Draining by recursion overflowed here, because a handler that
     * finishes synchronously hands back an already completed stage and the continuation runs on the
     * same stack.
     *
     * <p>The depth is well past what a default stack holds, and the recorded order is asserted so a
     * fix cannot buy depth by giving up the delivery order that spec 32 7 pins to the pumping
     * thread.
     */
    @Test
    void drainRunsAQueueDeeperThanTheStackInOrder() {
        int depth = 20_000;
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        List<Integer> order = new ArrayList<>();
        for (int index = 0; index < depth; index++) {
            int position = index;
            //  A synchronous item: its stage is already completed when the
            //  drain receives it. That is the shape that recursed.
            queue.add(() -> order.add(position));
        }

        queue.drainAsync().toCompletableFuture().join();

        assertEquals(depth, order.size());
        assertEquals(0, queue.pendingCallbacks());
        for (int index = 0; index < depth; index++) {
            assertEquals(index, order.get(index));
        }
    }

    /**
     * A failed callback completion is reported without preventing later callbacks from starting.
     */
    @Test
    void drainObservesFailedItemAndStartsTheRest() {
        List<ZLinkStreamError> errors = new ArrayList<>();
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue(errors::add);
        AtomicInteger handled = new AtomicInteger();
        RuntimeException failure = new IllegalStateException("handler failed");
        queue.add(handled::incrementAndGet);
        queue.addAsync(() -> CompletableFuture.failedFuture(failure));
        queue.add(handled::incrementAndGet);

        CompletableFuture<Void> drained = queue.drainAsync().toCompletableFuture();

        assertTrue(drained.isDone());
        assertEquals(2, handled.get());
        assertEquals(0, queue.pendingCallbacks());
        assertEquals(ZLinkStreamErrorCode.USER_CALLBACK_FAILED, errors.get(0).code());
    }

    /** An incomplete callback cannot transfer later dispatch work to its completion thread. */
    @Test
    void drainStartsNextWhilePreviousCompletionIsPending() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        List<String> order = new ArrayList<>();
        CompletableFuture<Void> gate = new CompletableFuture<>();
        queue.add(() -> order.add("first"));
        queue.addAsync(
                () -> {
                    order.add("pending");
                    return gate;
                });
        queue.add(() -> order.add("last"));

        CompletableFuture<Void> drained = queue.drainAsync().toCompletableFuture();

        assertEquals(List.of("first", "pending", "last"), order);
        assertTrue(drained.isDone());

        gate.complete(null);

        assertTrue(drained.isDone());
        assertEquals(List.of("first", "pending", "last"), order);
        assertEquals(0, queue.pendingCallbacks());
    }

    @Test
    void connectionEndCompletesWaitersWithoutReentrantModification() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiter =
                new CompletableFuture<>();

        queue.awaitMessage("Push", ignored -> true, waiter);

        queue.connectionEnded();

        assertTrue(waiter.isCompletedExceptionally());
        assertEquals(0, queue.pendingCallbacks());
        //  Nothing ever arrived on this queue, so the count is still 0.
        assertEquals(0, queue.receivedCount("Push"));
    }

    /**
     * Spec 32 10: a message that receivedCount reports is already observable to dispatch and
     * waitFor. A waiter predicate runs while the arrival is being routed, so it must not see the
     * count ahead of the queued message.
     */
    @Test
    void receivedCountNeverRunsAheadOfTheQueuedMessage() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        List<String> observedWhileRouting = new ArrayList<>();
        queue.awaitMessage(
                "Push",
                ignored -> {
                    observedWhileRouting.add(
                            "count="
                                    + queue.receivedCount("Push")
                                    + " queued="
                                    + queue.pendingCallbacks());
                    return false;
                },
                new CompletableFuture<>());

        queue.addMessage(
                message("body"),
                () -> () -> CompletableFuture.completedFuture(null),
                () -> 1,
                false);

        assertEquals(List.of("count=0 queued=0"), observedWhileRouting);
        assertEquals(1, queue.receivedCount("Push"));
        assertEquals(1, queue.pendingCallbacks());
    }

    @Test
    void packetWithoutSelectedHandlerStaysQueuedForWait() throws Exception {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        queue.addMessage(message("kept"), () -> null, () -> 1, false);
        queue.drainAsync().toCompletableFuture().get(1, TimeUnit.SECONDS);
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> received =
                new CompletableFuture<>();
        queue.awaitMessage("Push", ignored -> true, received);
        assertEquals("Push", received.get(1, TimeUnit.SECONDS).packetName());
    }

    private static ZLinkStreamMessage<ZLinkStreamEncodedPayload> message(String body) {
        return new ZLinkStreamMessage<>(
                "Push",
                new ZLinkStreamEncodedPayload("Push", Message.from(body), Map.of()),
                Map.of());
    }

    @Test
    void handlerlessManualMessageRemainsAvailableToWaitFor() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        0,
                                        null,
                                        "Late",
                                        Map.of(),
                                        null),
                                TcpStreamConnectorTestServer.bytes("queued"))
                        .join();

                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.receivedCount("Late") == 1);
                var message =
                        connector
                                .waitFor("Late")
                                .timeout(Duration.ofSeconds(1))
                                .submit()
                                .toCompletableFuture()
                                .get();
                try {
                    assertEquals(
                            "queued",
                            new String(
                                    message.payload().payload().toByteArray(),
                                    StandardCharsets.UTF_8));
                } finally {
                    message.payload().payload().close();
                }
                //  The wait surface consumed the message; spec 32 10
                //  keeps the arrival counted.
                assertEquals(1, connector.receivedCount("Late"));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    private static ZLinkStreamEncodedPayload payload(String packetName, String body) {
        return new ZLinkStreamEncodedPayload(packetName, Message.from(body), Map.of());
    }
}
