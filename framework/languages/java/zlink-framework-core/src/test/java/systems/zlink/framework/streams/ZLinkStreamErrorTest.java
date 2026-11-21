package systems.zlink.framework.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import java.util.Arrays;
import java.util.Optional;

final class ZLinkStreamErrorTest {
    @Test
    void reasonsUseCoreValuesRatherThanOrdinals() {
        assertArrayEquals(
                new int[] {0, 3, 4, 5},
                Arrays.stream(ZLinkDisconnectReason.values())
                        .mapToInt(ZLinkDisconnectReason::value)
                        .toArray());
    }

    @Test
    void compatibilityConstructorAndUnknownHaveDifferentPresence() {
        var absent = new ZLinkStreamError(ZLinkStreamSessionError.TRANSPORT_ERROR, "closed");
        var unknown =
                new ZLinkStreamError(
                        ZLinkStreamSessionError.TRANSPORT_ERROR,
                        "closed",
                        Optional.of(ZLinkDisconnectReason.UNKNOWN));
        assertEquals(Optional.empty(), absent.disconnectReason());
        assertEquals(0, unknown.disconnectReason().orElseThrow().value());
        assertThrows(
                NullPointerException.class,
                () ->
                        new ZLinkStreamError(
                                ZLinkStreamSessionError.TRANSPORT_ERROR, "closed", null));
    }
}
