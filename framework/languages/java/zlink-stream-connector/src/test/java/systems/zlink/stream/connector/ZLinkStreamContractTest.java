package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.ServerSocket;
import java.net.URI;
import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

/**
 * The regression items common connector spec 32 12 adds for error delivery,
 * reconnect delay, the close-reason read surface and the received count.
 */
final class ZLinkStreamContractTest {

    //  --- 9.2: the receiving side reads which of the thirteen codes it is ---

    @Test
    void optionValidationFailuresCarryTheirCode() {
        //  One value outside its permitted range.
        assertEquals(
            ZLinkStreamErrorCode.VALIDATION_FAILED,
            assertThrows(ZLinkStreamException.class,
                () -> ZLinkStreamConnectorFactory.create(new ZLinkStreamConnectorOptions(
                    URI.create("tcp://127.0.0.1:7000"),
                    ZLinkStreamDispatchMode.MANUAL,
                    Duration.ZERO,
                    1))).errorCode());

        //  A disagreement between two options, not a value out of range.
        assertEquals(
            ZLinkStreamErrorCode.CONFIGURATION_ERROR,
            assertThrows(ZLinkStreamException.class,
                () -> ZLinkStreamConnectorFactory.create(
                    heartbeatOptions(Duration.ofSeconds(5), Duration.ofSeconds(1)))).errorCode());
    }

    @Test
    void observationSurfaceViolationsAreValidationFailed() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                ConnectorTestAwait.await(connector.connect());

                //  waitFor that never sees its packet.
                CompletionException waitFailure = assertThrows(
                    CompletionException.class,
                    () -> connector.waitFor("Never")
                        .timeout(Duration.ofMillis(50))
                        .submit()
                        .toCompletableFuture()
                        .join());
                assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    codeOf(waitFailure));

                //  expectNone without a window, and waitForSequence without
                //  an expectation, are both caller mistakes on the surface.
                assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(ZLinkStreamException.class,
                        () -> connector.expectNone("Never").submit()).errorCode());
                assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    assertThrows(ZLinkStreamException.class,
                        () -> connector.waitForSequence("Never").submit()).errorCode());

                //  expectNone whose packet does arrive inside the window.
                CompletableFuture<Void> expectNone = connector
                    .expectNone("Push")
                    .within(Duration.ofSeconds(2))
                    .submit()
                    .toCompletableFuture();
                server.sendAsync(send("Push"), TcpStreamConnectorTestServer.bytes("x")).join();
                assertEquals(
                    ZLinkStreamErrorCode.VALIDATION_FAILED,
                    codeOf(assertThrows(CompletionException.class, expectNone::join)));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void sendingWhileDisconnectedIsDisconnected() {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
        assertEquals(
            ZLinkStreamErrorCode.DISCONNECTED,
            assertThrows(ZLinkStreamException.class,
                () -> connector.send(payload("Ping", "hello")).submit()).errorCode());
    }

    //  --- 6: the wait between reconnect attempts ---

    @Test
    void reconnectWaitStaysBetweenHalfAndAllOfTheBaseDelay() {
        Duration base = Duration.ofMillis(400);
        //  The random source is a parameter, so the two ends of the window
        //  are checked exactly instead of sampled.
        assertEquals(Duration.ofMillis(200), ZLinkStreamReconnectDelay.jittered(base, () -> 0.0));
        assertEquals(Duration.ofMillis(400), ZLinkStreamReconnectDelay.jittered(base, () -> 1.0));
        assertEquals(Duration.ofMillis(300), ZLinkStreamReconnectDelay.jittered(base, () -> 0.5));

        //  A source that misbehaves still cannot push the wait outside it.
        for (double value : new double[] {-1.0, 2.0, Double.NaN}) {
            Duration waited = ZLinkStreamReconnectDelay.jittered(base, () -> value);
            assertTrue(waited.compareTo(Duration.ofMillis(200)) >= 0, "under the window: " + waited);
            assertTrue(waited.compareTo(base) <= 0, "over the window: " + waited);
        }

        for (int i = 0; i < 200; i++) {
            Duration waited = ZLinkStreamReconnectDelay.jittered(base);
            assertTrue(waited.compareTo(Duration.ofMillis(200)) >= 0, "under the window: " + waited);
            assertTrue(waited.compareTo(base) <= 0, "over the window: " + waited);
        }
    }

    @Test
    void reconnectBaseDelayGrowsByTheBackoffFactorAndStopsAtTheMaximum() {
        ZLinkStreamConnectorConfiguration.Reconnect reconnect =
            new ZLinkStreamConnectorConfiguration.Reconnect(
                true, 5, Duration.ofMillis(250), Duration.ofMillis(1000), 2.0);

        Duration first = Duration.ofMillis(250);
        Duration second = ZLinkStreamReconnectDelay.nextBase(first, reconnect);
        Duration third = ZLinkStreamReconnectDelay.nextBase(second, reconnect);
        Duration fourth = ZLinkStreamReconnectDelay.nextBase(third, reconnect);
        Duration fifth = ZLinkStreamReconnectDelay.nextBase(fourth, reconnect);

        assertEquals(Duration.ofMillis(500), second);
        assertEquals(Duration.ofMillis(1000), third);
        assertEquals(Duration.ofMillis(1000), fourth);
        assertEquals(Duration.ofMillis(1000), fifth);
    }

    @Test
    void exhaustedReconnectAttemptsEndDisconnectedAndRunTheDisconnectHandler()
        throws Exception {
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
            AtomicInteger disconnects = new AtomicInteger();
            CountDownLatch exhausted = new CountDownLatch(1);
            connector.onDisconnected(event -> {
                disconnects.incrementAndGet();
                if (connector.state() == ZLinkStreamConnectionState.DISCONNECTED) {
                    exhausted.countDown();
                }
                return CompletableFuture.completedFuture(null);
            });

            ConnectorTestAwait.await(connector.connect());
            server.close();

            assertTrue(exhausted.await(10, TimeUnit.SECONDS),
                "the disconnect handler never ran with the connector Disconnected");
            assertEquals(ZLinkStreamConnectionState.DISCONNECTED, connector.state());
            assertTrue(disconnects.get() >= 1);
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    //  --- 6.2: the close-reason read surface ---

    @Test
    void closeReasonIsEmptyUntilAConnectionEnds() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                assertEquals(Optional.empty(), connector.closeReason());
                ConnectorTestAwait.await(connector.connect());
                assertEquals(Optional.empty(), connector.closeReason());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
            assertEquals(
                Optional.of(ZLinkStreamCloseReason.CLIENT_CLOSE),
                connector.closeReason());
        }
    }

    @Test
    void closeReasonSurvivesAFailedFirstConnect() throws Exception {
        int port = reservePort();
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofMillis(200),
                1,
                Duration.ofMillis(200),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                false,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0));
        try {
            assertEquals(Optional.empty(), connector.closeReason());
            assertThrows(Exception.class,
                () -> ConnectorTestAwait.await(connector.connect()));
            //  Spec 32 6.2/9: a connect that never succeeded still leaves a
            //  reason, and the impact table makes it TransportError.
            assertEquals(
                Optional.of(ZLinkStreamCloseReason.TRANSPORT_ERROR),
                connector.closeReason());
        } finally {
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void reconnectingDoesNotClearTheLastCloseReason() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(1),
                ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
                Duration.ofMillis(500),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                true,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0));
        try {
            ConnectorTestAwait.await(connector.connect());
            //  The test server only adopts an accepted socket once it is
            //  used, and closeCurrentSocket() closes the adopted one.
            server.sendAsync(send("Push"), TcpStreamConnectorTestServer.bytes("one")).join();
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.receivedCount("Push") == 1);
            server.closeCurrentSocket();

            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.closeReason().isPresent());
            assertEquals(
                Optional.of(ZLinkStreamCloseReason.TRANSPORT_ERROR),
                connector.closeReason());

            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.state() == ZLinkStreamConnectionState.CONNECTED);
            //  Reconnecting does not erase the reason the previous
            //  connection ended with (spec 32 6.2).
            assertEquals(
                Optional.of(ZLinkStreamCloseReason.TRANSPORT_ERROR),
                connector.closeReason());
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    //  --- 10: the received count ---

    @Test
    void receivedCountCountsArrivalsInBothDispatchModes() throws Exception {
        for (ZLinkStreamDispatchMode mode : List.of(
            ZLinkStreamDispatchMode.MANUAL, ZLinkStreamDispatchMode.IMMEDIATE)) {
            try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
                ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(server.options(mode));
                try {
                    AtomicInteger handled = new AtomicInteger();
                    connector.on("Push", message -> {
                        message.payload().payload().close();
                        handled.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    });
                    ConnectorTestAwait.await(connector.connect());
                    assertEquals(0, connector.receivedCount("Push"));

                    server.sendAsync(send("Push"),
                        TcpStreamConnectorTestServer.bytes("one")).join();
                    server.sendAsync(send("Push"),
                        TcpStreamConnectorTestServer.bytes("two")).join();

                    TcpStreamConnectorTestServer.awaitCondition(
                        () -> connector.receivedCount("Push") == 2);

                    ConnectorTestAwait.await(connector.dispatch());
                    TcpStreamConnectorTestServer.awaitCondition(() -> handled.get() == 2);

                    //  Spec 32 10: the count is of what arrived, so
                    //  consuming does not lower it and the dispatch mode
                    //  does not change it.
                    assertEquals(2, connector.receivedCount("Push"), "mode " + mode);
                    assertEquals(0, connector.receivedCount("Never"));
                } finally {
                    ConnectorTestAwait.await(connector.close());
                }
            }
        }
    }

    @Test
    void receivedCountRestartsWhenAConnectionIsEstablished() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(1),
                ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
                Duration.ofMillis(500),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                true,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0));
        try {
            connector.on("Push", message -> {
                message.payload().payload().close();
                return CompletableFuture.completedFuture(null);
            });
            ConnectorTestAwait.await(connector.connect());
            server.sendAsync(send("Push"), TcpStreamConnectorTestServer.bytes("one")).join();
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.receivedCount("Push") == 1);

            server.closeCurrentSocket();
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.state() == ZLinkStreamConnectionState.CONNECTED
                    && connector.receivedCount("Push") == 0);

            //  Spec 32 10: a reconnect is a new connection, so the count
            //  starts again at 0.
            assertEquals(0, connector.receivedCount("Push"));
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    @Test
    void establishingAConnectionClearsWhatThePreviousOneLeftUnconsumed() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.MANUAL,
                Duration.ofSeconds(1),
                ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
                Duration.ofMillis(500),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                true,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0));
        try {
            ConnectorTestAwait.await(connector.connect());
            //  Manual dispatch and no handler, so the message stays in the
            //  receive message queue.
            server.sendAsync(send("Push"), TcpStreamConnectorTestServer.bytes("before")).join();
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.receivedCount("Push") == 1
                    && connector.pendingDispatchCount() == 1);

            server.closeCurrentSocket();
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.state() == ZLinkStreamConnectionState.CONNECTED
                    && connector.receivedCount("Push") == 0);

            //  Spec 32 10: the count and the queue describe the same
            //  connection. Keeping the message while the count restarts
            //  would let waitFor hand back a packet from before the drop as
            //  if it belonged to the new connection.
            assertEquals(0, connector.pendingDispatchCount());
            assertEquals(
                ZLinkStreamErrorCode.VALIDATION_FAILED,
                codeOf(assertThrows(CompletionException.class,
                    () -> connector.waitFor("Push")
                        .timeout(Duration.ofMillis(300))
                        .submit()
                        .toCompletableFuture()
                        .join())));
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    @Test
    void aWaitLeftOverFromTheEndedConnectionIsDisconnected() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.MANUAL,
                Duration.ofSeconds(1),
                ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
                Duration.ofMillis(500),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                true,
                Duration.ofMillis(10),
                Duration.ofMillis(20),
                2.0));
        try {
            ConnectorTestAwait.await(connector.connect());
            CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiting = connector
                .waitFor("Push")
                .timeout(Duration.ofSeconds(5))
                .submit()
                .toCompletableFuture();

            //  The test server only adopts an accepted socket once it is
            //  used, and closeCurrentSocket() closes the adopted one.
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));
            server.closeCurrentSocket();

            //  Spec 32 10.1: the condition did not fail, the place to
            //  observe it went away.
            assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                codeOf(assertThrows(CompletionException.class, waiting::join)));
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    //  --- 10.1.1: a wait ends when the connection it observed ends ---

    /**
     * Spec 32 10.1.1: the wait is released when its connection ends, not
     * when the next connection is established. With reconnect off there is
     * no next connection, so a release bound to it would leave the wait
     * hanging until its own timeout, which ends as VALIDATION_FAILED.
     */
    @Test
    void aWaitEndsAsDisconnectedWhenTheConnectionEndsWithoutAReconnect() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                new ZLinkStreamConnectorOptions(
                    server.endpoint(),
                    ZLinkStreamDispatchMode.MANUAL,
                    Duration.ofSeconds(1),
                    1,
                    Duration.ofMillis(500),
                    64 * 1024,
                    false,
                    Duration.ofMillis(100),
                    Duration.ofMillis(300),
                    false,
                    Duration.ofMillis(10),
                    Duration.ofMillis(20),
                    2.0));
            try {
                ConnectorTestAwait.await(connector.connect());
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiting = connector
                    .waitFor("Push")
                    .timeout(Duration.ofSeconds(5))
                    .submit()
                    .toCompletableFuture();
                assertFalse(server.hasAdditionalConnection(Duration.ZERO));

                long startedAt = System.nanoTime();
                server.closeCurrentSocket();

                assertEquals(
                    ZLinkStreamErrorCode.DISCONNECTED,
                    codeOf(assertThrows(CompletionException.class, waiting::join)));
                //  Released by the ending, not by the 5 s wait timeout.
                assertTrue(
                    System.nanoTime() - startedAt < TimeUnit.SECONDS.toNanos(2),
                    "the wait waited for its own timeout");
                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.state() == ZLinkStreamConnectionState.DISCONNECTED);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    /**
     * Spec 32 10.1.1: the wait is released when its connection ends, before
     * the reconnect that follows has produced the next connection. The
     * reconnect delay is long enough that no second connection exists when
     * the wait ends.
     */
    @Test
    void aWaitEndsAsDisconnectedBeforeTheReconnectSucceeds() throws Exception {
        int port = reservePort();
        TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer(port);
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            new ZLinkStreamConnectorOptions(
                URI.create("tcp://127.0.0.1:" + port),
                ZLinkStreamDispatchMode.MANUAL,
                Duration.ofSeconds(1),
                ZLinkStreamConnectorOptions.UNLIMITED_RECONNECT_ATTEMPTS,
                Duration.ofMillis(500),
                64 * 1024,
                false,
                Duration.ofMillis(100),
                Duration.ofMillis(300),
                true,
                Duration.ofSeconds(3),
                Duration.ofSeconds(3),
                1.0));
        try {
            ConnectorTestAwait.await(connector.connect());
            CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> waiting = connector
                .waitFor("Push")
                .timeout(Duration.ofSeconds(5))
                .submit()
                .toCompletableFuture();
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));

            long startedAt = System.nanoTime();
            server.closeCurrentSocket();

            assertEquals(
                ZLinkStreamErrorCode.DISCONNECTED,
                codeOf(assertThrows(CompletionException.class, waiting::join)));
            //  The reconnect waits at least half of its 3 s base delay, so a
            //  release inside 1 s happened at the ending, not at the next
            //  connection - which does not exist yet.
            assertTrue(
                System.nanoTime() - startedAt < TimeUnit.SECONDS.toNanos(1),
                "the wait outlived the ending of its connection");
            //  The release runs before the state moves on, so the state is
            //  awaited rather than read at the instant the wait ended.
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.state() == ZLinkStreamConnectionState.RECONNECTING);
            assertFalse(server.hasAdditionalConnection(Duration.ZERO));
        } finally {
            ConnectorTestAwait.await(connector.close());
            server.close();
        }
    }

    //  --- 6: one ending, one disconnect notification ---

    @Test
    void aHeartbeatTimeoutEndsTheConnectionOnceAndKeepsItsReason() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                new ZLinkStreamConnectorOptions(
                    server.endpoint(),
                    ZLinkStreamDispatchMode.IMMEDIATE,
                    Duration.ofSeconds(1),
                    1,
                    Duration.ofMillis(500),
                    64 * 1024,
                    true,
                    Duration.ofMillis(50),
                    Duration.ofMillis(200),
                    false,
                    Duration.ofMillis(10),
                    Duration.ofMillis(20),
                    2.0));
            try {
                AtomicInteger disconnects = new AtomicInteger();
                connector.onDisconnected(event -> {
                    disconnects.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());

                //  The test server never answers a ping, so the heartbeat
                //  runs out. The receive loop sees the same transport go
                //  away, and both threads reach the ending.
                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.state() == ZLinkStreamConnectionState.DISCONNECTED);
                Thread.sleep(300);

                //  Spec 32 6: the transport being lost is one event, so the
                //  handler runs once for it however many threads observed
                //  it. Spec 32 6.2: the staged reason is consumed once, so
                //  it is not replaced by TransportError.
                assertEquals(1, disconnects.get());
                assertEquals(
                    Optional.of(ZLinkStreamCloseReason.HEARTBEAT_TIMEOUT),
                    connector.closeReason());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  --- 13: the synchronous diagnostics surface ---

    @Test
    void diagnosticsLevelChangesFromInsideACallbackWithoutWaiting() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                connector.on("Push", message -> {
                    message.payload().payload().close();
                    //  Spec 32 13: the synchronous surface must not wait for
                    //  its own completion, so this call inside a dispatch
                    //  callback returns rather than deadlocking.
                    connector.setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF);
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(send("Push"),
                    TcpStreamConnectorTestServer.bytes("one")).join();
                TcpStreamConnectorTestServer.awaitCondition(
                    () -> connector.pendingDispatchCount() == 1);

                ConnectorTestAwait.await(connector.dispatch());

                assertEquals(ZLinkStreamDiagnosticsLevel.OFF, connector.diagnosticsLevel());
                assertEquals(
                    ZLinkStreamDiagnosticsLevel.OFF,
                    connector.options().diagnosticsLevel());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void connectorInterfaceDeclaresTheContractSurface() throws Exception {
        assertNotNull(ZLinkStreamConnector.class.getMethod("closeReason"));
        assertEquals(
            void.class,
            ZLinkStreamConnector.class
                .getMethod("setDiagnosticsLevel", ZLinkStreamDiagnosticsLevel.class)
                .getReturnType());
        assertEquals(
            java.util.concurrent.CompletionStage.class,
            ZLinkStreamConnector.class
                .getMethod("setDiagnosticsLevelAsync", ZLinkStreamDiagnosticsLevel.class)
                .getReturnType());
        //  The instrumentation the client connector no longer owns.
        assertThrows(ClassNotFoundException.class, () -> Class.forName(
            "systems.zlink.stream.connector.ZLinkConnectorMetrics"));
        assertFalse(ZLinkStreamException.class.isAssignableFrom(IllegalArgumentException.class));
        assertTrue(RuntimeException.class.isAssignableFrom(ZLinkStreamException.class));
    }

    private static ZLinkStreamErrorCode codeOf(Throwable failure) {
        Throwable current = failure;
        while (current != null && !(current instanceof ZLinkStreamException)) {
            current = current.getCause();
        }
        assertNotNull(current, "expected a ZLinkStreamException in " + failure);
        return ((ZLinkStreamException) current).errorCode();
    }

    private static ZLinkStreamConnectorOptions heartbeatOptions(
        Duration interval,
        Duration timeout) {
        return new ZLinkStreamConnectorOptions(
            URI.create("tcp://127.0.0.1:7000"),
            ZLinkStreamDispatchMode.MANUAL,
            Duration.ofSeconds(1),
            1,
            Duration.ofSeconds(1),
            64 * 1024,
            true,
            interval,
            timeout,
            true,
            Duration.ofMillis(10),
            Duration.ofMillis(20),
            2.0);
    }

    private static ZLinkStreamWireProtocol.Header send(String name) {
        return new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND,
            ZLinkStreamWireProtocol.CODEC_RAW,
            0,
            null,
            name,
            Map.of(),
            null);
    }

    private static ZLinkStreamEncodedPayload payload(String packetName, String body) {
        return new ZLinkStreamEncodedPayload(packetName, Message.from(body), Map.of());
    }

    private static int reservePort() throws Exception {
        try (ServerSocket socket = new ServerSocket(0)) {
            return socket.getLocalPort();
        }
    }
}
