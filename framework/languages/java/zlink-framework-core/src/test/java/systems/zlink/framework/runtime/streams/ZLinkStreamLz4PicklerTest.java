package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

final class ZLinkStreamLz4PicklerTest {
    @Test
    void unpickleRejectsDecodedPayloadAboveDefaultLimitBeforeAllocation() {
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkStreamLz4Pickler.unpickle(new byte[] {(byte) 0xc0, 0x01, 0x00, 0x01, 0x00}));
    }

    @Test
    void unpickleRejectsDecodedPayloadAboveExplicitLimitBeforeAllocation() {
        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkStreamLz4Pickler.unpickle(new byte[] {0x40, 0x03}, 2));
    }
}
