package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class LifecycleTest {
    @Test
    void publicLifecycleSurfaceContainsOnlyContractMethods() {
        assertThrows(NoSuchMethodException.class, () ->
            ZLinkStreamConnector.class.getMethod("disconnect"));
        assertThrows(NoSuchMethodException.class, () ->
            ZLinkStreamConnector.class.getMethod("reconnect"));
    }

    @Test
    void concurrentConnectCallsShareOneTransportAttempt() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                CompletableFuture<Void> first = connector.connect().submit().toCompletableFuture();
                CompletableFuture<Void> second = connector.connect().submit().toCompletableFuture();

                CompletableFuture.allOf(first, second).get(5, TimeUnit.SECONDS);

                assertFalse(server.hasAdditionalConnection(Duration.ofMillis(200)));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void connectDuringAutomaticReconnectWaitsForThatAttempt() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(1),
                3,
                false,
                Duration.ofSeconds(1),
                Duration.ofSeconds(5),
                Duration.ofMillis(100)));
            try {
                List<ZLinkStreamConnectionState> states = new ArrayList<>();
                connector.onConnectionStateChanged(state -> {
                    states.add(state);
                    return CompletableFuture.completedFuture(null);
                });

                ConnectorTestAwait.await(connector.connect());
                assertFalse(server.hasAdditionalConnection(Duration.ZERO));
                server.closeCurrentSocket();
                TcpStreamConnectorTestServer.awaitCondition(() ->
                    connector.state() == ZLinkStreamConnectionState.RECONNECTING);

                ConnectorTestAwait.await(connector.connect());

                assertTrue(connector.isConnected());
                assertFalse(server.hasAdditionalConnection(Duration.ofMillis(200)));
                assertEquals(List.of(
                    ZLinkStreamConnectionState.CONNECTING,
                    ZLinkStreamConnectionState.CONNECTED,
                    ZLinkStreamConnectionState.RECONNECTING,
                    ZLinkStreamConnectionState.CONNECTED), states);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void automaticReconnectFailsAfterConfiguredMaxAttempts() throws Exception {
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer();
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(1),
            2,
            false,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            Duration.ofMillis(10)));
        try {
            ConnectorTestAwait.await(connector.connect());
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));
            server.close();
            TcpStreamConnectorTestServer.awaitCondition(() ->
                connector.state() == ZLinkStreamConnectionState.RECONNECTING);

            assertThrows(Exception.class, () -> ConnectorTestAwait.await(connector.connect()));

            assertEquals(ZLinkStreamConnectionState.DISCONNECTED, connector.state());
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    @Test
    void unlimitedAutomaticReconnectWaitsForALateServer() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnectorOptions options = new ZLinkStreamConnectorOptions(
            java.net.URI.create("tcp://127.0.0.1:" + port),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(1),
            ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
            Duration.ofMillis(100),
            64 * 1024,
            false,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            true,
            Duration.ofMillis(10),
            Duration.ofMillis(20),
            1.0);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(options);
        TcpStreamConnectorTestServer replacement = null;
        try {
            ConnectorTestAwait.await(connector.connect());
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));
            server.close();
            TcpStreamConnectorTestServer.awaitCondition(() ->
                connector.state() == ZLinkStreamConnectionState.RECONNECTING);

            CompletableFuture<TcpStreamConnectorTestServer> lateServer = CompletableFuture.supplyAsync(
                () -> {
                    try {
                        return new TcpStreamConnectorTestServer(port);
                    } catch (Exception error) {
                        throw new CompletionException(error);
                    }
                },
                CompletableFuture.delayedExecutor(120, TimeUnit.MILLISECONDS));
            ConnectorTestAwait.await(connector.connect());
            replacement = lateServer.get(5, TimeUnit.SECONDS);

            assertTrue(connector.isConnected());
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
            if (replacement != null) {
                replacement.close();
            }
        }
    }

    @Test
    void closeWhileAutomaticallyReconnectingKeepsConnectorClosed() throws Exception {
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer();
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofSeconds(1),
            2,
            false,
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            Duration.ofMillis(100)));
        try {
            ConnectorTestAwait.await(connector.connect());
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));
            server.close();
            TcpStreamConnectorTestServer.awaitCondition(() ->
                connector.state() == ZLinkStreamConnectionState.RECONNECTING);
            CompletableFuture<Void> reconnect = connector.connect().submit().toCompletableFuture();

            ConnectorTestAwait.await(connector.close());

            CompletionException error = assertThrows(CompletionException.class, reconnect::join);
            assertTrue(error.getCause() instanceof IllegalStateException);
            assertEquals(ZLinkStreamConnectionState.CLOSED, connector.state());
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    @Test
    void connectorStartsInCreatedStateBeforeFirstConnectAttempt() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                assertEquals(ZLinkStreamConnectionState.CREATED, connector.state());
                assertFalse(connector.isConnected());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void requestTimeoutFailsPendingRequestsWithTimeoutCause() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                ZLinkStreamConnectorFactory.create(server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
            ConnectorTestAwait.await(connector.connect());

            CompletionException ex = assertThrows(CompletionException.class, () ->
                connector.request(payload("MissingReply", "hello"))
                    .timeout(Duration.ofMillis(10))
                    .submit()
                    .toCompletableFuture()
                    .join());

            assertTrue(ex.getCause() instanceof java.util.concurrent.TimeoutException);
            assertEquals(0, connector.pendingDispatchCount());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void heartbeatSendsReservedControlPing() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
                 ZLinkStreamDispatchMode.IMMEDIATE,
                 Duration.ofSeconds(1),
                 1,
                 true,
                 Duration.ofMillis(25),
                 Duration.ofMillis(500),
                 Duration.ofMillis(10)));
            try {
            ConnectorTestAwait.await(connector.connect());

            TcpStreamConnectorTestServer.ReceivedFrame frame =
                server.readFrameAsync().get(1, TimeUnit.SECONDS);

            assertEquals(ZLinkStreamWireProtocol.KIND_CONTROL, frame.header().kind());
            assertEquals("$zlink.heartbeat.ping", frame.header().name());
            assertEquals(0, frame.payload().length);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void inboundHeartbeatPingReceivesPongWhenHeartbeatDisabled() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
                 ZLinkStreamDispatchMode.IMMEDIATE,
                 Duration.ofSeconds(1),
                 1,
                 false,
                 Duration.ofMillis(25),
                 Duration.ofMillis(500),
                 Duration.ofMillis(10)));
            try {
            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(control("$zlink.heartbeat.ping"), new byte[0]).join();

            TcpStreamConnectorTestServer.ReceivedFrame frame =
                server.readFrameAsync().get(1, TimeUnit.SECONDS);

            assertEquals(ZLinkStreamWireProtocol.KIND_CONTROL, frame.header().kind());
            assertEquals("$zlink.heartbeat.pong", frame.header().name());
            assertEquals(0, frame.payload().length);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void heartbeatTimeoutFailsPendingRequestsWithTimeoutCause() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(server.options(
                 ZLinkStreamDispatchMode.IMMEDIATE,
                 Duration.ofSeconds(5),
                 1,
                 true,
                 Duration.ofMillis(20),
                 Duration.ofMillis(60),
                 Duration.ofMillis(10)));
            try {
            ConnectorTestAwait.await(connector.connect());

            CompletionException ex = assertThrows(CompletionException.class, () ->
                connector.request(payload("MissingReply", "hello"))
                    .submit()
                    .toCompletableFuture()
                    .join());

            assertTrue(ex.getCause() instanceof java.util.concurrent.TimeoutException);
            assertTrue(ex.getCause().getMessage().contains("Heartbeat"));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void reconnectEnabledRejectsZeroMaxAttempts() {
        ZLinkStreamConnectorOptions options = new ZLinkStreamConnectorOptions(
            java.net.URI.create("tcp://127.0.0.1:1"),
            ZLinkStreamDispatchMode.IMMEDIATE,
            Duration.ofMillis(100),
            0,
            Duration.ofMillis(100),
            64 * 1024,
            false,
            Duration.ofMillis(25),
            Duration.ofMillis(100),
            true,
            Duration.ofMillis(10),
            Duration.ofMillis(20),
            2.0);

        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamConnectorFactory.create(options));
    }

    private static ZLinkStreamEncodedPayload payload(String packetName, String body) {
        return new ZLinkStreamEncodedPayload(
            packetName,
            Message.from(body),
            Map.of());
    }

    private static int reservePort() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) {
            return socket.getLocalPort();
        }
    }

    private static ZLinkStreamWireProtocol.Header control(String name) {
        return new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_CONTROL,
            ZLinkStreamWireProtocol.CODEC_RAW,
            0,
            null,
            name,
            Map.of(),
            null);
    }

}
