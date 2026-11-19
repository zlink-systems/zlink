package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

import java.nio.ByteBuffer;
import java.util.Map;

final class ZLinkChannelEnvelopeGoldenTest {
    @Test
    void kindAndFlowOriginValuesStayPinnedToTheProtoContract() {
        assertEquals(1, ZLinkChannelEnvelope.KIND_REQUEST);
        assertEquals(2, ZLinkChannelEnvelope.KIND_RESPONSE);
        assertEquals(3, ZLinkChannelEnvelope.KIND_COMMAND);
        assertEquals(4, ZLinkChannelEnvelope.KIND_PUBLISH);
        assertEquals(5, ZLinkChannelEnvelope.KIND_ERROR);
        assertEquals(1, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.INBOUND));
        assertEquals(2, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.TIMER));
        assertEquals(3, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.APPLICATION));
        assertEquals(4, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.LIFECYCLE));
    }

    @Test
    void generatedReplyHeaderEchoesRequestIdentityAndFlow() {
        var request =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "orders",
                        "PlaceOrder",
                        "application/json",
                        "corr-1",
                        "raw deadline bytes",
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        "018f2f1d-5d52-7b70-8f08-13fecf6f6abc",
                        ZLinkFlowOrigin.APPLICATION);
        var decoded =
                ZLinkChannelEnvelope.decodeHeader(
                        ByteBuffer.wrap(
                                ZLinkChannelEnvelope.encodeHeaderBytes(
                                        ZLinkChannelEnvelope.reply(request))),
                        true);
        assertEquals(2, decoded.kind());
        assertEquals(request.correlationId(), decoded.correlationId());
        assertEquals(request.flowId(), decoded.flowId());
        assertEquals(request.flowOrigin(), decoded.flowOrigin());
        assertNull(decoded.deadline());
    }

    @Test
    void metadataWriterUsesUtf8ByteOrderingAndErrorCodeNamesStayCanonical() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        3,
                        "c",
                        "m",
                        "x",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of("z", "last", "é", "utf8", "a", "first"),
                        null,
                        null);
        byte[] wire = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        String hex = java.util.HexFormat.of().formatHex(wire);
        assertTrue(hex.indexOf("0a0161") < hex.indexOf("0a017a"));
        assertNotEquals(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                ZLinkChannelEnvelope.errorKindFromCode("internal_failure"));
        assertEquals(
                "data_lost", ZLinkChannelEnvelope.errorCodeName(ZLinkFrameworkErrorKind.DATA_LOST));
    }
}
