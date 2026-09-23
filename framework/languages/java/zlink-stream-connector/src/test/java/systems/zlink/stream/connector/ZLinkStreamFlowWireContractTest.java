package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.util.Arrays;
import java.util.Map;
import java.util.concurrent.CompletableFuture;

final class ZLinkStreamFlowWireContractTest {
    @Test
    void inboundFlowIsStructurallyCheckedAndDiscarded() {
        var header =
                new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        "Move",
                        Map.of(),
                        null,
                        "not-a-uuid-xxxxxxxxxxxxxxxxxxxxxxxxx",
                        255);
        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);
        ZLinkStreamWireProtocol.Header decoded = ZLinkStreamWireProtocol.decodeHeader(encoded);
        assertNull(decoded.flowId());
        assertEquals(0, decoded.flowOrigin());
        assertThrows(
                IllegalArgumentException.class,
                () ->
                        ZLinkStreamWireProtocol.decodeHeader(
                                Arrays.copyOf(encoded, encoded.length - 1)));
    }

    @Test
    void outboundSendFromInboundHandlerHasNoFlowFlag() throws Exception {
        try (TcpStreamConnectorTestServer server = new TcpStreamConnectorTestServer()) {
            ZLinkStreamConnector connector =
                    ZLinkStreamConnectorFactory.create(
                            server.options(ZLinkStreamDispatchMode.IMMEDIATE));
            try {
                connector.on(
                        "Inbound",
                        message -> {
                            message.payload().payload().close();
                            connector
                                    .send(
                                            new ZLinkStreamEncodedPayload(
                                                    "Outbound", Message.from("reply"), Map.of()))
                                    .submit();
                            return CompletableFuture.completedFuture(null);
                        });
                ConnectorTestAwait.await(connector.connect());
                var outbound = server.readFrameAsync();
                server.sendAsync(
                                new ZLinkStreamWireProtocol.Header(
                                        ZLinkStreamWireProtocol.KIND_SEND,
                                        ZLinkStreamWireProtocol.CODEC_RAW,
                                        0,
                                        null,
                                        "Inbound",
                                        Map.of(),
                                        null,
                                        "not-a-uuid-xxxxxxxxxxxxxxxxxxxxxxxxx",
                                        255),
                                TcpStreamConnectorTestServer.bytes("request"))
                        .join();
                TcpStreamConnectorTestServer.ReceivedFrame sent = outbound.join();
                assertEquals(0, sent.header().flags() & ZLinkStreamWireProtocol.FLAG_HAS_FLOW_ID);
                assertNull(sent.header().flowId());
                assertTrue(
                        Arrays.stream(ZLinkStreamMessage.class.getRecordComponents())
                                .noneMatch(component -> component.getName().startsWith("flow")));
            } finally {
                ConnectorTestAwait.await(connector.close());
            }
        }
    }
}
