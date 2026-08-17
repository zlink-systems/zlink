package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

final class ZLinkSpotFlowFrameTest {
    private static final String FLOW_ID = "018f2f1d-5d52-7b70-8f08-13fecf6f6abc";

    @Test
    void decodeReadsFlowFrameAfterPayload() {
        List<Message> parts = List.of(
            Message.from("Packet".getBytes(StandardCharsets.UTF_8)),
            Message.from(new byte[] {1}),
            flowFrame(FLOW_ID, "INBOUND"));
        try {
            var decoded = ZLinkSpotFlowFrame.decode(parts);
            assertEquals(FLOW_ID, decoded.flowId());
            assertEquals(ZLinkFlowOrigin.INBOUND, decoded.origin());
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void decodeReadsFlowFrameBehindContentTypeFrame() {
        //  ZLinkSpotRouteMessages.encode places the flow frame after the
        //  content-type frame; the decoder must not lose the inbound pair.
        List<Message> parts = List.of(
            Message.from("Packet".getBytes(StandardCharsets.UTF_8)),
            Message.from(new byte[] {1}),
            Message.from("__zlink.content_type\napplication/json"
                .getBytes(StandardCharsets.UTF_8)),
            flowFrame(FLOW_ID, "APPLICATION"));
        try {
            var decoded = ZLinkSpotFlowFrame.decode(parts);
            assertEquals(FLOW_ID, decoded.flowId());
            assertEquals(ZLinkFlowOrigin.APPLICATION, decoded.origin());
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void decodeReturnsNullWithoutFlowFrame() {
        List<Message> parts = List.of(
            Message.from("Packet".getBytes(StandardCharsets.UTF_8)),
            Message.from(new byte[] {1}));
        try {
            assertNull(ZLinkSpotFlowFrame.decode(parts));
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void decodeRejectsMalformedFlowAsProtocolError() {
        for (Message frame : List.of(
            flowFrame("018f2f1d-5d52-6b70-8f08-13fecf6f6abc", "INBOUND"),
            flowFrame("018F2F1D-5D52-7B70-8F08-13FECF6F6ABC", "INBOUND"),
            flowFrame(FLOW_ID, "NOT_AN_ORIGIN"),
            Message.from("__zlink.flow\nonly-one-field"
                .getBytes(StandardCharsets.UTF_8)))) {
            List<Message> parts = List.of(
                Message.from("Packet".getBytes(StandardCharsets.UTF_8)),
                Message.from(new byte[] {1}),
                frame);
            try {
                ZLinkFrameworkException failure = assertThrows(
                    ZLinkFrameworkException.class,
                    () -> ZLinkSpotFlowFrame.decode(parts));
                assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
            } finally {
                Message.closeAll(parts);
            }
        }
    }

    private static Message flowFrame(String flowId, String origin) {
        return Message.from(("__zlink.flow\n" + flowId + "\n" + origin)
            .getBytes(StandardCharsets.UTF_8));
    }
}
