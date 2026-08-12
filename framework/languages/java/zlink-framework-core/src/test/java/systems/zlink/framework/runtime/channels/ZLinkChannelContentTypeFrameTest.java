package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;

final class ZLinkChannelContentTypeFrameTest {
    @Test
    void decodePreservesNonCanonicalWireValueForStrictRegistryValidation() {
        try (Message envelope = Message.from(new byte[] {1});
             Message payload = Message.from(new byte[] {2});
             Message contentType = ZLinkChannelContentTypeFrame.encode(
                 " \tApplication/X-Base\t ")) {
            assertEquals(
                " \tApplication/X-Base\t ",
                ZLinkChannelContentTypeFrame.decode(
                    List.of(envelope, payload, contentType)));
        }
    }
}
