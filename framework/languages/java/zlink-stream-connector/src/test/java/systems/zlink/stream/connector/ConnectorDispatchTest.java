package systems.zlink.stream.connector;
import java.time.Duration;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ConnectorDispatchTest {
    @Test
    void dispatchModeSurfaceUsesContractNames() {
        assertEquals(
            List.of("MANUAL", "IMMEDIATE"),
            Arrays.stream(ZLinkStreamDispatchMode.values())
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
            List<ZLinkStreamConnectionState> states = new ArrayList<>();
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
    void cancelledQueuedWaiterClosesTheMessageItCannotReceive() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
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
        //  The message did arrive, so spec 32 10 keeps it counted even
        //  though the cancelled waiter could not take it.
        assertEquals(1, queue.receivedCount("Push"));
    }

    /**
     * Spec 32 7: a {@code Manual} queue grows until the user pumps, so the
     * drain has to survive a queue longer than the stack. Draining by
     * recursion overflowed here, because a handler that finishes
     * synchronously hands back an already completed stage and the
     * continuation runs on the same stack.
     *
     * <p>The depth is well past what a default stack holds, and the recorded
     * order is asserted so a fix cannot buy depth by giving up the delivery
     * order that spec 32 7 pins to the pumping thread.
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
        assertEquals(0, queue.size());
        for (int index = 0; index < depth; index++) {
            assertEquals(index, order.get(index));
        }
    }

    /**
     * The drain must still stop at the first failure and report it, which is
     * what the recursive chain did. Spec 32 7 has the connector run the
     * registered handlers; it does not have it swallow their failures.
     */
    @Test
    void drainStopsAtTheFirstFailedItemAndKeepsTheRest() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        AtomicInteger handled = new AtomicInteger();
        RuntimeException failure = new IllegalStateException("handler failed");
        queue.add(handled::incrementAndGet);
        queue.addAsync(() -> CompletableFuture.failedFuture(failure));
        queue.add(handled::incrementAndGet);

        CompletableFuture<Void> drained = queue.drainAsync().toCompletableFuture();

        assertTrue(drained.isCompletedExceptionally());
        assertEquals(1, handled.get());
        assertEquals(1, queue.size());
        assertSame(
            failure,
            assertThrows(CompletionException.class, drained::join).getCause());
    }

    /**
     * An item whose stage completes later must not be overtaken: the drain
     * resumes only after that stage finishes, and it resumes on whichever
     * thread completed it, so a pumping thread keeps running its own items.
     */
    @Test
    void drainResumesOnlyAfterAPendingItemCompletes() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        List<String> order = new ArrayList<>();
        CompletableFuture<Void> gate = new CompletableFuture<>();
        queue.add(() -> order.add("first"));
        queue.addAsync(() -> {
            order.add("pending");
            return gate;
        });
        queue.add(() -> order.add("last"));

        CompletableFuture<Void> drained = queue.drainAsync().toCompletableFuture();

        assertEquals(List.of("first", "pending"), order);
        assertFalse(drained.isDone());

        gate.complete(null);

        assertTrue(drained.isDone());
        assertEquals(List.of("first", "pending", "last"), order);
        assertEquals(0, queue.size());
    }

    @Test
    void clearCompletesWaitersWithoutReentrantModification() {
        ZLinkStreamDispatchQueue queue = new ZLinkStreamDispatchQueue();
        CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiter =
            new CompletableFuture<>();

        queue.awaitMessage("Push", ignored -> true, waiter);

        queue.clear();

        assertTrue(waiter.isCompletedExceptionally());
        assertEquals(0, queue.size());
        //  Nothing ever arrived on this queue, so the count is still 0.
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
                    .timeout(Duration.ofSeconds(1))
                    .submit()
                    .toCompletableFuture()
                    .get();
                try {
                    assertEquals("queued", new String(
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
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(body),
            Map.of());
    }
}
