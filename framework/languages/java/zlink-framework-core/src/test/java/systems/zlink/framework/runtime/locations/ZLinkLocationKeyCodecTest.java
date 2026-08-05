package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;

final class ZLinkLocationKeyCodecTest {
    @Test
    void encodesTheRemainingFrameworkOwnedFanoutKeyCanonically() {
        assertEquals(
            "6:events4:0a2b",
            ZLinkLocationKeyCodec.encodeFanoutPublisherKey(
                new ZLinkFanoutPublisherDescriptorKey(
                    "events",
                    RoutingId.from(new byte[] {0x0a, 0x2b}))));
    }
}
