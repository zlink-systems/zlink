package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

final class ZLinkActorSpotJoinCallTest {
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
