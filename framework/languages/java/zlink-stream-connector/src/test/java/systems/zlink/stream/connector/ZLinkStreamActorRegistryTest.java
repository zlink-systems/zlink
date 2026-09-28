package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.lang.reflect.Field;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkStreamActorRegistryTest {
    @Test
    void closeNotifiesActorUnboundBeforeConnectionStateAndDisconnected() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());
            ConnectorTestAwait.await(connector.dispatch());
            registry(connector).bound(boundControl(7, "player-a"));

            List<String> order = new ArrayList<>();
            connector.onActorUnbound(
                    actor -> {
                        order.add("unbound");
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onConnectionStateChanged(
                    state -> {
                        if (state == ZLinkStreamConnectionState.CLOSED) {
                            order.add("state");
                        }
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onDisconnected(
                    event -> {
                        order.add("disconnected");
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.close());
            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(List.of("unbound", "state", "disconnected"), order);
        }
    }

    @Test
    void boundAndUnboundOwnTheSnapshotCallbacksAndClosedHandleValidation() throws Exception {
        ZLinkStreamConnector connector = connector();
        ZLinkStreamActorRegistry registry = registry(connector);
        CompletableFuture<ZLinkStreamActor> bound = new CompletableFuture<>();
        CompletableFuture<ZLinkStreamActor> unbound = new CompletableFuture<>();
        connector.onActorBound(
                actor -> {
                    bound.complete(actor);
                    return CompletableFuture.completedFuture(null);
                });
        connector.onActorUnbound(
                actor -> {
                    unbound.complete(actor);
                    return CompletableFuture.completedFuture(null);
                });

        registry.bound(boundControl(7, "player-a"));
        ZLinkStreamActor actor = connector.actor("player-a").orElseThrow();

        assertEquals(1, connector.actors().size());
        assertSame(actor, connector.actors().getFirst());
        connector.dispatch().submit().toCompletableFuture().join();
        assertSame(actor, bound.join());

        registry.unbound(unboundControl(7));
        assertTrue(connector.actors().isEmpty());
        assertFalse(actor.isBound());
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, sendFailure(actor));
        connector.dispatch().submit().toCompletableFuture().join();
        assertSame(actor, unbound.join());
        registry.bound(boundControl(7, "player-b"));
        assertTrue(connector.actor("player-b").orElseThrow().isBound());
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, sendFailure(actor));
        ZLinkStreamException rawTypedFailure =
                assertThrows(ZLinkStreamException.class, () -> actor.send((Object) payload()));
        assertEquals(ZLinkStreamErrorCode.VALIDATION_FAILED, rawTypedFailure.errorCode());
    }

    @Test
    void validThenMalformedActorControlUsesWireDecodeFailureAndClosesConnection() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(
                                    ZLinkStreamDispatchMode.IMMEDIATE,
                                    java.time.Duration.ofSeconds(1),
                                    1,
                                    false,
                                    java.time.Duration.ofSeconds(1),
                                    java.time.Duration.ofSeconds(1),
                                    java.time.Duration.ofMillis(10)));
            AtomicReference<ZLinkStreamError> error = new AtomicReference<>();
            CountDownLatch disconnected = new CountDownLatch(1);
            connector.onErrorReceived(
                    received -> {
                        error.set(received);
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onDisconnected(
                    ignored -> {
                        disconnected.countDown();
                        return CompletableFuture.completedFuture(null);
                    });

            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            controlHeader(ZLinkStreamActorRegistry.BOUND),
                            boundControl(7, "player-a"))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.actor("player-a").isPresent());
            assertEquals(ZLinkStreamConnectionState.CONNECTED, connector.state());

            server.sendAsync(
                            controlHeader(ZLinkStreamActorRegistry.BOUND),
                            new byte[] {1, 0, 8, 2, (byte) 0xc3, 0x28})
                    .join();

            assertTrue(disconnected.await(5, TimeUnit.SECONDS));
            assertEquals(ZLinkStreamErrorCode.FRAME_DECODE_FAILED, error.get().code());
            assertTrue(server.hasAdditionalConnection(java.time.Duration.ofSeconds(5)));
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void duplicateAndUnknownActorControlsAreRejectedByTheRegistry() throws Exception {
        ZLinkStreamActorRegistry registry = registry(connector());
        registry.bound(boundControl(7, "player-a"));

        assertThrows(
                IllegalArgumentException.class, () -> registry.bound(boundControl(7, "player-b")));
        assertThrows(
                IllegalArgumentException.class, () -> registry.bound(boundControl(8, "player-a")));
        assertThrows(IllegalArgumentException.class, () -> registry.unbound(unboundControl(9)));
    }

    @Test
    void manualDispatchPreservesActorPacketBeforeUnboundNotification() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            controlHeader(ZLinkStreamActorRegistry.BOUND),
                            boundControl(7, "player-a"))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.actor("player-a").isPresent());
            ZLinkStreamActor actor = connector.actor("player-a").orElseThrow();
            CompletableFuture<String> received = new CompletableFuture<>();
            CompletableFuture<Void> unbound = new CompletableFuture<>();
            actor.on(
                    "Ping",
                    message -> {
                        try {
                            received.complete(message.actorId());
                        } finally {
                            message.payload().payload().close();
                        }
                        return CompletableFuture.completedFuture(null);
                    });
            connector.onActorUnbound(
                    ignored -> {
                        unbound.complete(null);
                        return CompletableFuture.completedFuture(null);
                    });

            server.sendAsync(actorPacketHeader(7, "Ping"), new byte[] {1}).join();
            server.sendAsync(controlHeader(ZLinkStreamActorRegistry.UNBOUND), unboundControl(7))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(() -> connector.actors().isEmpty());

            assertFalse(actor.isBound());
            ConnectorTestAwait.await(connector.dispatch());
            assertEquals("player-a", received.join());
            assertTrue(unbound.isDone());
            assertFalse(actor.isBound());
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void manualDispatchSkipsActorHandlerUnregisteredAfterPacketWasQueued() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.MANUAL));
            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(
                            controlHeader(ZLinkStreamActorRegistry.BOUND),
                            boundControl(7, "player-a"))
                    .join();
            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.actor("player-a").isPresent());
            ZLinkStreamActor actor = connector.actor("player-a").orElseThrow();
            java.util.concurrent.atomic.AtomicInteger calls =
                    new java.util.concurrent.atomic.AtomicInteger();
            AutoCloseable registration =
                    actor.on(
                            "Ping",
                            message -> {
                                calls.incrementAndGet();
                                message.payload().payload().close();
                                return CompletableFuture.completedFuture(null);
                            });

            server.sendAsync(actorPacketHeader(7, "Ping"), new byte[] {1}).join();
            TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);
            registration.close();
            ConnectorTestAwait.await(connector.dispatch());

            assertEquals(0, calls.get());
            ConnectorTestAwait.await(connector.close());
        }
    }

    private static ZLinkStreamConnector connector() {
        return ZLinkStreamConnectorFactory.create(
                ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
    }

    private static ZLinkStreamActorRegistry registry(ZLinkStreamConnector connector)
            throws Exception {
        Field field = DefaultZLinkStreamConnector.class.getDeclaredField("actorRegistry");
        field.setAccessible(true);
        return (ZLinkStreamActorRegistry) field.get(connector);
    }

    /**
     * Spec 32 5.6, 7, 10: an Actor handle receive registration follows the same rule as the
     * connector one. A packet no handler takes stays queued, and a handle handler registered after
     * it arrived receives it at the next dispatch, in both dispatch modes.
     */
    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.EnumSource(ZLinkStreamDispatchMode.class)
    void anActorHandlerRegisteredAfterThePacketArrivedReceivesIt(ZLinkStreamDispatchMode mode)
            throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(server.options(mode));
            try {
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(
                                controlHeader(ZLinkStreamActorRegistry.BOUND),
                                boundControl(7, "player-a"))
                        .join();
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.actor("player-a").isPresent());
                ZLinkStreamActor actor = connector.actor("player-a").orElseThrow();
                server.sendAsync(actorPacketHeader(7, "Late"), new byte[] {9}).join();
                TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.receivedCount("Late") == 1);

                CompletableFuture<String> received = new CompletableFuture<>();
                actor.on(
                        "Late",
                        message -> {
                            received.complete(message.actorId());
                            return CompletableFuture.completedFuture(null);
                        });
                ConnectorTestAwait.await(connector.dispatch());

                assertEquals("player-a", received.get(5, TimeUnit.SECONDS));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    private static byte[] boundControl(int slot, String actorId) {
        byte[] id = actorId.getBytes(StandardCharsets.UTF_8);
        return ByteBuffer.allocate(4 + id.length)
                .put((byte) 1)
                .putShort((short) slot)
                .put((byte) id.length)
                .put(id)
                .array();
    }

    private static byte[] unboundControl(int slot) {
        return ByteBuffer.allocate(3).put((byte) 1).putShort((short) slot).array();
    }

    private static ZLinkStreamWireProtocol.Header controlHeader(String name) {
        return new ZLinkStreamWireProtocol.Header(
                ZLinkStreamWireProtocol.KIND_CONTROL,
                ZLinkStreamWireProtocol.CODEC_RAW,
                0,
                null,
                name,
                Map.of(),
                null);
    }

    private static ZLinkStreamWireProtocol.Header actorPacketHeader(int slot, String name) {
        return new ZLinkStreamWireProtocol.Header(
                ZLinkStreamWireProtocol.KIND_SEND,
                ZLinkStreamWireProtocol.CODEC_RAW,
                ZLinkStreamWireProtocol.FLAG_HAS_ACTOR_SLOT,
                null,
                name,
                Map.of(),
                null,
                null,
                0,
                slot);
    }

    private static ZLinkStreamEncodedPayload payload() {
        return new ZLinkStreamEncodedPayload(
                "Ping", Message.from(new byte[] {1}), Map.of(), ZLinkStreamCodec.RAW);
    }

    /** Spec 32 9.2: a Send that is not accepted fails its stage; submit() does not throw. */
    private static ZLinkStreamErrorCode sendFailure(ZLinkStreamActor actor) {
        CompletableFuture<Void> send = actor.send(payload()).submit().toCompletableFuture();
        CompletionException failure = assertThrows(CompletionException.class, send::join);
        return ((ZLinkStreamException) failure.getCause()).errorCode();
    }
}
