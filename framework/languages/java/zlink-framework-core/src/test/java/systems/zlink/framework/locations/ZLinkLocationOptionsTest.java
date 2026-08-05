package systems.zlink.framework.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import org.junit.jupiter.api.Test;

final class ZLinkLocationOptionsTest {
    @Test
    void objectRoutingAndRelocationDefaultsMatchTheExactContract() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();

        assertEquals(Duration.ofSeconds(15), options.routeCacheMaxAge());
        assertEquals(
            Duration.ofSeconds(30),
            options.messageFollowDuration());
        assertEquals(64, options.maxActiveOutboundRelocations());
        assertEquals(64, options.maxActiveInboundRelocations());
        assertEquals(8, options.maxConcurrentRelocationCaptures());
        assertEquals(8, options.maxConcurrentRelocationRestores());
        assertEquals(
            256L * 1024 * 1024,
            options.maxRelocationPayloadInFlightBytes());
    }

    @Test
    void routeCacheCanBeDisabledButCannotOutliveForwardingSafetyMargin() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ZERO);
        assertEquals(Duration.ZERO, options.routeCacheMaxAge());

        assertThrows(
            IllegalArgumentException.class,
            () -> new ZLinkLocationOptions()
                .setRouteCacheMaxAge(Duration.ofSeconds(26)));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMessageFollowDuration(
                Duration.ofSeconds(-1)));
    }

    @Test
    void relocationBudgetsMustBePositive() {
        ZLinkLocationOptions options = new ZLinkLocationOptions();

        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMaxActiveOutboundRelocations(0));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMaxActiveInboundRelocations(0));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMaxConcurrentRelocationCaptures(0));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMaxConcurrentRelocationRestores(0));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setMaxRelocationPayloadInFlightBytes(0));
    }
}
