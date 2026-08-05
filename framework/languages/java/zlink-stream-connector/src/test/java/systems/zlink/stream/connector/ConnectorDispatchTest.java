package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ConnectorDispatchTest {
    @Test
    void dispatchModeSurfaceUsesContractNames() {
        assertEquals(
            java.util.List.of("MANUAL", "IMMEDIATE"),
            java.util.Arrays.stream(ZLinkStreamDispatchMode.values())
                .map(Enum::name)
                .toList());
    }

    @Test
    void manualDispatchWaitsForMessageCallbackCompletion() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                CompletableFuture<Void> callback = new CompletableFuture<>();
                connector.on("Slow", message -> {
                    message.payload().payload().close();
                    return callback;
                });
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Slow",
                        Map.of(),
                        null),
                    TcpStreamConnectorTestServer.bytes("body")).join();
                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);

                CompletableFuture<Void> dispatched = connector.dispatch()
                    .submit()
                    .toCompletableFuture();
                assertFalse(dispatched.isDone());
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
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.MANUAL));
            java.util.List<ZLinkStreamConnectionState> states = new java.util.ArrayList<>();
            AtomicInteger disconnected = new AtomicInteger();
            connector.onConnectionStateChanged(state -> {
                states.add(state);
                return CompletableFuture.completedFuture(null);
            });
            connector.onDisconnected(event -> {
                disconnected.incrementAndGet();
                return CompletableFuture.completedFuture(null);
            });

            ConnectorTestAwait.await(connector.connect());
            assertEquals(java.util.List.of(), states);
            ConnectorTestAwait.await(connector.dispatch());
            assertEquals(
                java.util.List.of(
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
    void dispatch_invokesCallback() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
            AtomicInteger handled = new AtomicInteger();
            connector.on("Ping", message -> {
                handled.incrementAndGet();
                assertEquals("Ping", message.packetName());
                assertEquals("42", message.metadata().get("seq"));
                assertEquals("hello", new String(
                    message.payload().payload().toByteArray(),
                    StandardCharsets.UTF_8));
                message.payload().payload().close();
                return CompletableFuture.completedFuture(null);
            });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(new ZLinkStreamWireProtocol.Header(
                    ZLinkStreamWireProtocol.KIND_SEND,
                    ZLinkStreamWireProtocol.CODEC_RAW,
                    ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
                    null,
                    "Ping",
                    Map.of("seq", "42"),
            null),
                TcpStreamConnectorTestServer.bytes("hello")).join();

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
    void dispatchQueueDropsNewestReceivedMessageWhenBounded() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue(1);
        java.util.List<String> handled = new java.util.ArrayList<>();

        queue.addMessage(message("push-0"),
            () -> { handled.add("push-0"); return java.util.concurrent.CompletableFuture.completedFuture(null); },
            () -> true, false);
        queue.addMessage(message("push-1"),
            () -> { handled.add("push-1"); return java.util.concurrent.CompletableFuture.completedFuture(null); },
            () -> true, false);
        queue.addMessage(message("push-2"),
            () -> { handled.add("push-2"); return java.util.concurrent.CompletableFuture.completedFuture(null); },
            () -> true, false);

        assertEquals(1, queue.size());
        assertEquals(1, queue.receivedCount("Push"));

        queue.drainAsync().toCompletableFuture().join();

        assertEquals(java.util.List.of("push-0"), handled);
        assertEquals(0, queue.receivedCount("Push"));
    }

    @Test
    void dispatchQueueReportsEachDroppedMessage() {
        AtomicInteger dropped = new AtomicInteger();
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue(1, error -> {
            assertEquals(ZLinkStreamErrorCode.RECEIVED_MESSAGE_DROPPED, error.code());
            dropped.incrementAndGet();
        });

        queue.addMessage(message("first"),
            () -> CompletableFuture.completedFuture(null), () -> true, false);
        queue.addMessage(message("second"),
            () -> CompletableFuture.completedFuture(null), () -> true, false);

        assertEquals(1, dropped.get());
        assertEquals(1, queue.receivedCount("Push"));
    }

    @Test
    void processDropPublishesExactlyOnceAndPreservesOlderQueueItem() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL, 1));
            try {
                AtomicInteger dropped = new AtomicInteger();
                connector.onErrorReceived(error -> {
                    if (error.code() == ZLinkStreamErrorCode.RECEIVED_MESSAGE_DROPPED) {
                        dropped.incrementAndGet();
                    }
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());

                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Drop",
                        Map.of(),
                        null),
                    TcpStreamConnectorTestServer.bytes("first")).join();
                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Drop",
                        Map.of(),
                        null),
                    TcpStreamConnectorTestServer.bytes("second")).join();

                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.receivedCount("Drop") == 1
                        && connector.pendingDispatchCount() >= 2);
                ConnectorTestAwait.await(connector.dispatch());
                TcpStreamConnectorTestServer.awaitCondition(() -> dropped.get() == 1);
                assertEquals(1, dropped.get());
                assertEquals(1, connector.receivedCount("Drop"));
                var retained = connector.waitFor("Drop")
                    .timeout(java.time.Duration.ofSeconds(1))
                    .submit()
                    .toCompletableFuture()
                    .get();
                try {
                    assertEquals(
                        "first",
                        new String(
                            retained.payload().payload().toByteArray(),
                            StandardCharsets.UTF_8));
                } finally {
                    retained.payload().payload().close();
                }
                assertEquals(0, connector.receivedCount("Drop"));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void cancelledQueuedWaiterClosesTheMessageItCannotReceive() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue(1);
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> queued = message("queued");
        queue.addMessage(
            queued,
            () -> CompletableFuture.completedFuture(null),
            () -> true,
            false);

        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> cancelled =
            new CompletableFuture<>();
        assertTrue(cancelled.cancel(false));

        queue.awaitMessage("Push", ignored -> true, cancelled);

        assertEquals(0, queued.payload().payload().size());
        assertEquals(0, queue.receivedCount("Push"));
    }

    @Test
    void clearCompletesWaitersWithoutReentrantModification() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue(1);
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiter =
            new CompletableFuture<>();

        queue.awaitMessage("Push", ignored -> true, waiter);

        queue.clear();

        assertTrue(waiter.isCompletedExceptionally());
        assertEquals(0, queue.size());
        assertEquals(0, queue.receivedCount("Push"));
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
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Late",
                        Map.of(),
                        null),
                    TcpStreamConnectorTestServer.bytes("queued")).join();

                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);
                var message = connector.waitFor("Late")
                    .timeout(java.time.Duration.ofSeconds(1))
                    .submit()
                    .toCompletableFuture()
                    .get();
                try {
                    assertEquals("queued", new String(
                        message.payload().payload().toByteArray(),
                        java.nio.charset.StandardCharsets.UTF_8));
                } finally {
                    message.payload().payload().close();
                }
                assertEquals(0, connector.receivedCount("Late"));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    private static ZLinkStreamEncodedPayload payload(String packetName, String body) {
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(body),
            Map.of());
    }
}
