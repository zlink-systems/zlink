package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

final class ZLinkSessionClosingControlTest {
    @Test
    void serverDrainUsesFrozenVersionAndReasonCode() {
        byte[] encoded = ZLinkSessionClosingControl.serverDrain("");

        assertArrayEquals(new byte[] {1, 4, 0, 0}, encoded);
        assertEquals(1, ZLinkSessionClosingControl.VERSION);
    }
}
