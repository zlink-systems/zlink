package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

final class ZLinkActorSpotJoinCallTest {
    @Test
    void sourceCleanupSkipsTheAlreadyCompletedMessageFollowRouteLookup() {
        assertFalse(ZLinkActorSpotJoinCall.requiresMessageFollowTargetRoute(
            true,
            true,
            false));
        assertTrue(ZLinkActorSpotJoinCall.requiresMessageFollowTargetRoute(
            true,
            false,
            false));
        assertFalse(ZLinkActorSpotJoinCall.requiresMessageFollowTargetRoute(
            true,
            false,
            true));
    }

    @Test
    void deadlineSaturationDoesNotTreatNegativeNanoTimeAsOverflow() {
        assertEquals(
            Long.MIN_VALUE + 50L,
            ZLinkActorSpotJoinCall.saturatedDeadline(Long.MIN_VALUE, 50L));
        assertEquals(
            Long.MAX_VALUE,
            ZLinkActorSpotJoinCall.saturatedDeadline(
                Long.MAX_VALUE - 1L,
                2L));
    }
}
