package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;

import java.util.List;

final class ZLinkChannelFlowFrameTest {
    @Test
    void readsFlowOriginFromChannelHeader() {
        List<Message> parts = parts("018f2f1d-5d52-7b70-8f08-13fecf6f6abc", 1);
        try {
            var header = ZLinkChannelEnvelope.decodeDispatchHeader(parts, true);
            var decoded = ZLinkChannelFlowFrame.fromEnvelopeHeader(header);
            assertEquals(ZLinkFlowOrigin.INBOUND, decoded.origin());
        } finally {
            Message.closeAll(parts);
        }
    }

    @Test
    void rejectsUnspecifiedFlowOrigin() {
        List<Message> parts = parts("018f2f1d-5d52-7b70-8f08-13fecf6f6abc", 0);
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

    private static List<Message> parts(String flowId, int origin) {
        byte[] header =
                ChannelHeader.newBuilder()
                        .setKindValue(1)
                        .setFlowId(flowId)
                        .setFlowOriginValue(origin)
                        .build()
                        .toByteArray();
        return List.of(Message.from(header), Message.from(new byte[] {1}));
    }
}
