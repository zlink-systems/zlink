package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;

final class ZLinkStreamTestHelperTest {
    private final List<ZLinkStreamConnector> connectors = new ArrayList<>();

    @AfterEach
    void closeConnectors() throws Exception {
        for (ZLinkStreamConnector connector : connectors) {
            ConnectorTestAwait.await(connector.close());
        }
    }

    @Test
    void expectNonePassesOnlyWhenTheWindowHasNoMatchingMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            connector.expectNone("Notice")
                .within(Duration.ofMillis(25))
                .submit()
                .toCompletableFuture()
                .join();

            var unexpected = connector.expectNone("Notice")
                .within(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            send(server, "Notice", "unexpected");

            assertThrows(CompletionException.class, unexpected::join);
        }
    }

    @Test
    void timedOutWaiterDoesNotConsumeALaterMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            var timedOut = connector.waitFor("Late")
                .timeout(Duration.ofMillis(25))
                .submit()
                .toCompletableFuture();
            assertThrows(CompletionException.class, timedOut::join);

            send(server, "Late", "after-timeout");
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.pendingDispatchCount() == 1);

            var message = connector.waitFor("Late")
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture()
                .join();
            try {
                assertEquals("after-timeout", text(message));
            } finally {
                message.payload().payload().close();
            }
        }
    }

    @Test
    void cancelledWaiterDoesNotConsumeALaterMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            var cancelled = connector.waitFor("Cancelled")
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            assertTrue(cancelled.cancel(false));

            send(server, "Cancelled", "after-cancel");
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.pendingDispatchCount() == 1);

            var message = connector.waitFor("Cancelled")
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try {
                assertEquals("after-cancel", text(message));
            } finally {
                message.payload().payload().close();
            }
        }
    }

    @Test
    void cancelledExpectNoneDoesNotConsumeALaterMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            var cancelled = connector.expectNone("Cancelled")
                .within(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            assertTrue(cancelled.cancel(false));

            send(server, "Cancelled", "after-cancel");
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.pendingDispatchCount() == 1);

            var message = connector.waitFor("Cancelled")
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try {
                assertEquals("after-cancel", text(message));
            } finally {
                message.payload().payload().close();
            }
        }
    }

    @Test
    void cancelledSequenceDoesNotConsumeALaterMessage() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            var cancelled = connector.waitForSequence("Cancelled")
                .expect(message -> text(message).equals("first"))
                .expect(message -> text(message).equals("second"))
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            assertTrue(cancelled.cancel(false));

            send(server, "Cancelled", "after-cancel");
            TcpStreamConnectorTestServer.awaitCondition(
                () -> connector.pendingDispatchCount() == 1);

            var message = connector.waitFor("Cancelled")
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try {
                assertEquals("after-cancel", text(message));
            } finally {
                message.payload().payload().close();
            }
        }
    }

    @Test
    void waitForSequenceChecksEachMessageAgainstTheNextExpectation() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = connector(server);
            ConnectorTestAwait.await(connector.connect());

            var sequence = connector.waitForSequence("Notice")
                .expect(message -> text(message).equals("first"))
                .expect(message -> text(message).equals("second"))
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            send(server, "Notice", "first");
            send(server, "Notice", "second");

            List<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> messages = sequence.join();
            try {
                assertEquals(List.of("first", "second"), messages.stream()
                    .map(ZLinkStreamTestHelperTest::text)
                    .toList());
            } finally {
                messages.forEach(message -> message.payload().payload().close());
            }

            var outOfOrder = connector.waitForSequence("Notice")
                .expect(message -> text(message).equals("first"))
                .expect(message -> text(message).equals("second"))
                .timeout(Duration.ofSeconds(1))
                .submit()
                .toCompletableFuture();
            send(server, "Notice", "second");

            assertThrows(CompletionException.class, outOfOrder::join);
        }
    }

    @Test
    void assertionsPreserveFailureKindsAndRethrowNonTimeouts() {
        ZLinkStreamAssert.ensure(true, "condition should pass");
        assertThrows(IllegalStateException.class,
            () -> ZLinkStreamAssert.ensure(false, "condition failed"));

        ZLinkStreamError timeout = ZLinkStreamAssert.expectFailure(
            () -> { throw new TimeoutException("request timed out"); },
            "REQUEST_TIMEOUT");
        assertEquals(ZLinkStreamErrorCode.REQUEST_TIMEOUT, timeout.code());

        ZLinkStreamAssert.expectTimeout(
            () -> { throw new TimeoutException("request timed out"); });
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkStreamAssert.expectTimeout(
                () -> { throw new IllegalArgumentException("invalid"); }));
    }

    private ZLinkStreamConnector connector(TcpStreamConnectorTestServer server) {
        ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
        connectors.add(connector);
        return connector;
    }

    private static void send(
        TcpStreamConnectorTestServer server,
        String name,
        String value) {
        server.sendAsync(new ZLinkStreamWireProtocol.Header(
                ZLinkStreamWireProtocol.KIND_SEND,
                ZLinkStreamWireProtocol.CODEC_RAW,
                0,
                null,
                name,
                Map.of(),
                null),
            TcpStreamConnectorTestServer.bytes(value)).join();
    }

    private static String text(
        ZLinkStreamMessage<ZLinkStreamEncodedPayload> message) {
        return new String(
            message.payload().payload().toByteArray(),
            StandardCharsets.UTF_8);
    }
}
