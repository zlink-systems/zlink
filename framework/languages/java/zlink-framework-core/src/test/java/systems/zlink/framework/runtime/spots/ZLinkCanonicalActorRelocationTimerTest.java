package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;

import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;

final class ZLinkCanonicalActorRelocationTimerTest {
    @Test
    void standaloneActorRootPreservesLogicalTimers() {
        var timer = new ZLinkSpotTimerRelocationEnvelope.CanonicalTimer(
            "heartbeat",
            TestTimerHandler.class.getName(),
            1000,
            1,
            1,
            true,
            7,
            9,
            123456,
            new ZLinkSpotTimerRelocationEnvelope.CanonicalPending(
                8, 10, 123456, 2));
        byte[] timerEnvelope =
            ZLinkSpotTimerRelocationEnvelope.encodeCanonical(List.of(timer));
        UUID relocationId = UUID.randomUUID();

        byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
            relocationId,
            "actor-a",
            3,
            5,
            true,
            new byte[] {1, 2, 3},
            List.of(),
            timerEnvelope);
        var decoded = ZLinkCanonicalActorRelocationEnvelope.decode(
            root, relocationId, "actor-a", true);

        assertArrayEquals(timerEnvelope, decoded.timerEnvelope());
        assertEquals(
            1,
            ZLinkSpotTimerRelocationEnvelope
                .canonicalize(decoded.timerEnvelope()).size());
    }

    private static final class TestTimerHandler {
    }
}
