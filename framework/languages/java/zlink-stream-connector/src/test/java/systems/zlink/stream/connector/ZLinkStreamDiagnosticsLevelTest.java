package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ZLinkStreamDiagnosticsLevelTest {
    @Test
    void defaultsToErrorsAndWitherOverridesLevel() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnectorOptions options =
                server.options(ZLinkStreamDispatchMode.MANUAL);
            assertEquals(ZLinkStreamDiagnosticsLevel.ERRORS, options.diagnosticsLevel());
            assertEquals(
                ZLinkStreamDiagnosticsLevel.OFF,
                options.withDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF)
                    .diagnosticsLevel());
            assertEquals(
                ZLinkStreamDiagnosticsLevel.ERRORS,
                ZLinkStreamConnectorOptions.createDefault(server.endpoint())
                    .diagnosticsLevel());
        }
    }

    //  Server spec 26 §4.1 / common connector spec §13: a connector starts at
    //  the option-supplied default (Errors) and exposes it through the live
    //  read API without requiring the caller to inspect options().
    @Test
    void connectorDefaultsToErrorsThroughLiveReadApi() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                assertEquals(ZLinkStreamDiagnosticsLevel.ERRORS, connector.diagnosticsLevel());
                assertEquals(
                    ZLinkStreamDiagnosticsLevel.ERRORS, connector.options().diagnosticsLevel());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void setDiagnosticsLevelRejectsNull() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.MANUAL));
            try {
                assertThrows(NullPointerException.class, () -> connector.setDiagnosticsLevel(null));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Server spec 26 §4.1: application reads/changes the level without
    //  recreating the connector, and the change applies to processing points
    //  read after the call, not retroactively to already-built frames.
    @Test
    void liveOnToOffHidesFlowOnNextOutboundFrame() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                ConnectorTestAwait.await(connector.connect());
                assertEquals(ZLinkStreamDiagnosticsLevel.ERRORS, connector.diagnosticsLevel());

                var beforeOutbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "Before", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame before =
                    beforeOutbound.get(5, TimeUnit.SECONDS);
                assertNotNull(before.header().flowId());

                connector.setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF);
                assertEquals(ZLinkStreamDiagnosticsLevel.OFF, connector.diagnosticsLevel());
                assertEquals(ZLinkStreamDiagnosticsLevel.OFF, connector.options().diagnosticsLevel());

                var afterOutbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "After", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame after =
                    afterOutbound.get(5, TimeUnit.SECONDS);
                assertNull(after.header().flowId());
                assertEquals(
                    0,
                    after.header().flags() & ZLinkStreamWireProtocol.FLAG_HAS_FLOW_ID);
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void liveOffToOnRestoresFlowOnNextOutboundFrame() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE)
                    .withDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF));
            try {
                ConnectorTestAwait.await(connector.connect());
                assertEquals(ZLinkStreamDiagnosticsLevel.OFF, connector.diagnosticsLevel());

                var beforeOutbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "Before", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame before =
                    beforeOutbound.get(5, TimeUnit.SECONDS);
                assertNull(before.header().flowId());

                connector.setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.ERRORS);
                assertEquals(ZLinkStreamDiagnosticsLevel.ERRORS, connector.diagnosticsLevel());
                assertEquals(
                    ZLinkStreamDiagnosticsLevel.ERRORS, connector.options().diagnosticsLevel());

                var afterOutbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "After", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame after =
                    afterOutbound.get(5, TimeUnit.SECONDS);
                assertNotNull(after.header().flowId());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Common connector spec §13: "each processing point reads the current
    //  level once." Flipping the level from inside the handler must not
    //  retroactively change the flow fields of the message already decoded
    //  and dispatched for this frame; only the *next* frame observes the
    //  new level.
    @Test
    void inFlightInboundDispatchIsUnaffectedByLevelFlipInsideHandler() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> first =
                    new CompletableFuture<>();
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> second =
                    new CompletableFuture<>();
                connector.on("Flip", message -> {
                    message.payload().payload().close();
                    if (!first.isDone()) {
                        first.complete(message);
                        //  Flip mid-handler; must not retroactively affect the
                        //  already-decoded/dispatched message above.
                        connector.setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF);
                    } else {
                        second.complete(message);
                    }
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());
                assertEquals(ZLinkStreamDiagnosticsLevel.ERRORS, connector.diagnosticsLevel());

                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Flip",
                        Map.of(),
                        null,
                        ZLinkConnectorFlowIds.next(),
                        1),
                    TcpStreamConnectorTestServer.bytes("first")).join();
                ZLinkStreamMessage<ZLinkStreamEncodedPayload> firstMessage =
                    first.get(5, TimeUnit.SECONDS);
                assertNotNull(firstMessage.flowId());
                assertEquals(ZLinkStreamDiagnosticsLevel.OFF, connector.diagnosticsLevel());

                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Flip",
                        Map.of(),
                        null,
                        ZLinkConnectorFlowIds.next(),
                        1),
                    TcpStreamConnectorTestServer.bytes("second")).join();
                ZLinkStreamMessage<ZLinkStreamEncodedPayload> secondMessage =
                    second.get(5, TimeUnit.SECONDS);
                assertNull(secondMessage.flowId());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Regression guard for the single-read refactor: decodeHeader's
    //  validate-flow gate and dispatchToHandlers' flow-install gate must
    //  agree for a single frame. If they read the level independently, a
    //  live flip landing between the two reads could make one gate validate
    //  a corrupted flow id (throwing) while the other skips it, or vice
    //  versa. This sends a structurally valid but value-corrupted flow id
    //  frame once the level has already settled at Off.
    @Test
    void liveOffSkipsInboundFlowValidationConsistentlyAfterFlip() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> seen =
                    new CompletableFuture<>();
                connector.on("Corrupt", message -> {
                    message.payload().payload().close();
                    seen.complete(message);
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());
                connector.setDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF);

                ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
                    ZLinkStreamWireProtocol.KIND_SEND,
                    ZLinkStreamWireProtocol.CODEC_RAW,
                    0,
                    null,
                    "Corrupt",
                    Map.of(),
                    null,
                    ZLinkConnectorFlowIds.next(),
                    1);
                byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(header);
                //  Corrupt the UUIDv7 version nibble inside the 36-byte flow id.
                encodedHeader[encodedHeader.length - 23] = 'z';
                server.sendRawAsync(encodedHeader, TcpStreamConnectorTestServer.bytes("payload"))
                    .join();

                ZLinkStreamMessage<ZLinkStreamEncodedPayload> message =
                    seen.get(5, TimeUnit.SECONDS);
                assertNull(message.flowId());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Spec 27 §2 (D2): a one-way Send has no reply, so the connector creates
    //  no correlation_id and leaves flag 0x08 clear; the flow pair is still
    //  attached at the default (Errors) level.
    @Test
    void oneWaySendCarriesNoCorrelationIdAtDefaultLevel() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                ConnectorTestAwait.await(connector.connect());
                var outbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "OneWay", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame sent =
                    outbound.get(5, TimeUnit.SECONDS);
                assertNull(sent.header().correlationId());
                assertEquals(
                    0,
                    sent.header().flags()
                        & ZLinkStreamWireProtocol.FLAG_HAS_CORRELATION_ID);
                assertNotNull(sent.header().flowId());
                assertEquals(3, sent.header().flowOrigin());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void requestStillCarriesCorrelationId() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                ConnectorTestAwait.await(connector.connect());
                var outbound = server.readFrameAsync();
                connector.request(new ZLinkStreamEncodedPayload(
                    "Ask", Message.from("payload"), Map.of())).submit();
                TcpStreamConnectorTestServer.ReceivedFrame sent =
                    outbound.get(5, TimeUnit.SECONDS);
                assertNotNull(sent.header().correlationId());
                assertNotNull(sent.header().requestSeq());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Spec 27 §4 (D1): at Off the outbound frame carries no flow pair (flag
    //  0x10 clear) and no flow id is generated.
    @Test
    void offOutboundCarriesNoFlowPair() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE)
                    .withDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF));
            try {
                ConnectorTestAwait.await(connector.connect());
                var outbound = server.readFrameAsync();
                connector.send(new ZLinkStreamEncodedPayload(
                        "OneWay", Message.from("payload"), Map.of()))
                    .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
                TcpStreamConnectorTestServer.ReceivedFrame sent =
                    outbound.get(5, TimeUnit.SECONDS);
                assertNull(sent.header().flowId());
                assertEquals(0, sent.header().flowOrigin());
                assertEquals(
                    0,
                    sent.header().flags() & ZLinkStreamWireProtocol.FLAG_HAS_FLOW_ID);
                assertNull(sent.header().correlationId());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Spec 27 §4 (D1): at Off the inbound flow pair is neither validated nor
    //  installed as flow context; handlers observe no flow fields.
    @Test
    void offInboundInstallsNoFlowContext() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE)
                    .withDiagnosticsLevel(ZLinkStreamDiagnosticsLevel.OFF));
            try {
                CompletableFuture<ZLinkStreamMessage<ZLinkStreamEncodedPayload>> seen =
                    new CompletableFuture<>();
                connector.on("Inbound", message -> {
                    message.payload().payload().close();
                    seen.complete(message);
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());
                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Inbound",
                        Map.of(),
                        null,
                        ZLinkConnectorFlowIds.next(),
                        1),
                    TcpStreamConnectorTestServer.bytes("request")).join();
                ZLinkStreamMessage<ZLinkStreamEncodedPayload> message =
                    seen.get(5, TimeUnit.SECONDS);
                assertNull(message.flowId());
                assertNull(message.flowOrigin());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    //  Spec 27 §4 (D1): at Off the trace-only UUIDv7/origin validation is
    //  skipped while the structural 37-byte flow trailer length check stays.
    @Test
    void offSkipsFlowValidationButKeepsStructuralChecks() {
        var header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND,
            ZLinkStreamWireProtocol.CODEC_RAW,
            0,
            null,
            "Move",
            Map.of(),
            null,
            ZLinkConnectorFlowIds.next(),
            1);
        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);
        //  Corrupt the UUIDv7 version nibble inside the 36-byte flow id.
        encoded[encoded.length - 23] = 'z';
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkStreamWireProtocol.decodeHeader(encoded));
        ZLinkStreamWireProtocol.Header decoded =
            ZLinkStreamWireProtocol.decodeHeader(encoded, false);
        assertNotNull(decoded.flowId());

        //  A truncated flow trailer stays a structural error at Off.
        byte[] truncated = new byte[encoded.length - 5];
        System.arraycopy(encoded, 0, truncated, 0, truncated.length);
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkStreamWireProtocol.decodeHeader(truncated, false));
        assertTrue(encoded.length > 37);
    }
}
