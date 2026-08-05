package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

final class ZLinkSessionClosingControlTest {
    @Test
    void decodesServerDrainCloseReason() {
        assertEquals(
            ZLinkStreamCloseReason.SERVER_DRAIN,
            ZLinkSessionClosingControl.decode(new byte[] {1, 4, 0, 0}));
    }

    @Test
    void rejectsUnknownVersionAndReason() {
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkSessionClosingControl.decode(new byte[] {2, 4, 0, 0}));
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkSessionClosingControl.decode(new byte[] {1, 7, 0, 0}));
    }
}
