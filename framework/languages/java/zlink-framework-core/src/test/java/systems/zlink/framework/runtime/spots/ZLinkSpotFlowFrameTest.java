package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;

import java.util.List;

final class ZLinkSpotFlowFrameTest {
    private static final String FLOW_ID = "018f2f1d-5d52-7b70-8f08-13fecf6f6abc";

    @Test
    void readsFlowFromChannelHeader() {
        List<Message> parts = parts(header(FLOW_ID, 3));
        try {
            var decoded =
                    ZLinkSpotFlowFrame.fromEnvelopeHeader(
                            ZLinkChannelEnvelope.decodeDispatchHeader(parts, true));
            assertEquals(FLOW_ID, decoded.flowId());
            assertEquals(ZLinkFlowOrigin.APPLICATION, decoded.origin());
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void returnsNullWhenChannelHeaderHasNoFlow() {
        List<Message> parts = parts(header(null, 0));
        try {
            assertNull(
                    ZLinkSpotFlowFrame.fromEnvelopeHeader(
                            ZLinkChannelEnvelope.decodeDispatchHeader(parts, true)));
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void rejectsInvalidFlowIdAsProtocolError() {
        List<Message> parts = parts(header("018f2f1d-5d52-6b70-8f08-13fecf6f6abc", 3));
        try {
            ZLinkFrameworkException failure =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () -> ZLinkChannelEnvelope.decodeDispatchHeader(parts, true));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        } finally {
            Message.closeAll(parts);
        }
    }

    private static byte[] header(String flowId, int origin) {
        var builder = ChannelHeader.newBuilder().setKindValue(3).setMessageName("Move");
        if (flowId != null) builder.setFlowId(flowId).setFlowOriginValue(origin);
        return builder.build().toByteArray();
    }

    private static List<Message> parts(byte[] header) {
        return List.of(Message.from(header), Message.from(new byte[] {1}));
    }
}
