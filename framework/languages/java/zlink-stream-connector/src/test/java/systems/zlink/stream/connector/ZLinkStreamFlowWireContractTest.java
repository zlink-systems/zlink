package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;

final class ZLinkStreamFlowWireContractTest {
    @Test
    void connectorCreatesCanonicalApplicationFlowAndRoundTripsIt() {
        String flowId = ZLinkConnectorFlowIds.next();
        var header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND, ZLinkStreamWireProtocol.CODEC_JSON,
            0, null, "Move", Map.of(), "corr-1", flowId, 3);
        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);
        assertEquals(0xF2, Byte.toUnsignedInt(encoded[0]));
        ZLinkStreamWireProtocol.Header decoded = ZLinkStreamWireProtocol.decodeHeader(encoded);
        assertEquals(flowId, decoded.flowId());
        assertEquals(3, decoded.flowOrigin());
        assertEquals("corr-1", decoded.correlationId());
        assertTrue(flowId.matches(
            "[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}"));
    }

    @Test
    void outboundCallInsideInboundCallbackPreservesFlow() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector = ZLinkStreamConnectorFactory.create(
                server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                String inboundFlow = ZLinkConnectorFlowIds.next();
                connector.on("Inbound", message -> {
                    assertEquals(inboundFlow, message.flowId());
                    assertEquals(ZLinkFlowOrigin.INBOUND, message.flowOrigin());
                    message.payload().payload().close();
                    connector.send(new ZLinkStreamEncodedPayload(
                        "Outbound",
                        systems.zlink.contracts.messaging.Message.from("reply"),
                        Map.of())).submit();
                    return CompletableFuture.completedFuture(null);
                });
                ConnectorTestAwait.await(connector.connect());
                var outbound = server.readFrameAsync();
                server.sendAsync(new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Inbound",
                        Map.of(),
                        null,
                        inboundFlow,
                        1),
                    TcpStreamConnectorTestServer.bytes("request")).join();

                TcpStreamConnectorTestServer.ReceivedFrame sent = outbound.join();
                assertEquals(inboundFlow, sent.header().flowId());
                assertEquals(1, sent.header().flowOrigin());
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }

    @Test
    void rejectsLegacyAndUnknownMandatoryFormats() {
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkStreamWireProtocol.decodeHeader(new byte[] {1, 0, 0, 1, 'x'}));
        byte[] header = ZLinkStreamWireProtocol.encodeHeader(
            new ZLinkStreamWireProtocol.Header(1, 0, 0, null, "x", Map.of(), null));
        header[3] |= 0x20;
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkStreamWireProtocol.decodeHeader(header));
    }
}
