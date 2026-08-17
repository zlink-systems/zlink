package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

final class ZLinkChannelFlowFrameTest {
    @Test
    void tracingOnDecodeAcceptsOnlyCanonicalUuidV7() {
        List<Message> valid = parts(
            "018f2f1d-5d52-7b70-8f08-13fecf6f6abc",
            "INBOUND");
        try {
            var decoded = ZLinkChannelFlowFrame.decode(valid);
            assertEquals(ZLinkFlowOrigin.INBOUND, decoded.origin());
        } finally {
            Message.closeAll(valid);
        }

        for (String invalid : List.of(
            "018f2f1d-5d52-6b70-8f08-13fecf6f6abc",
            "018F2F1D-5D52-7B70-8F08-13FECF6F6ABC")) {
            List<Message> parts = parts(invalid, "INBOUND");
            try {
                assertThrows(
                    PayloadDecodeDispatchException.class,
                    () -> ZLinkChannelFlowFrame.decode(parts));
            } finally {
                Message.closeAll(parts);
            }
        }
    }

    private static List<Message> parts(String flowId, String origin) {
        return List.of(
            Message.from("Packet".getBytes(StandardCharsets.UTF_8)),
            Message.from(new byte[] {1}),
            Message.from(("__zlink.flow\n" + flowId + "\n" + origin)
                .getBytes(StandardCharsets.UTF_8)));
    }
}
