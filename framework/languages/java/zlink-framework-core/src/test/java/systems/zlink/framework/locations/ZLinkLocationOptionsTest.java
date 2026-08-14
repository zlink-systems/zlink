package systems.zlink.framework.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Duration;
import java.lang.reflect.InvocationTargetException;
import org.junit.jupiter.api.Test;

final class ZLinkLocationOptionsTest {
    @Test
    void sessionRelocationSealTimeoutHasTheExactRootLocationContract()
        throws Exception {
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        var getter = ZLinkLocationOptions.class.getMethod(
            "sessionRelocationSealTimeout");
        var setter = ZLinkLocationOptions.class.getMethod(
            "setSessionRelocationSealTimeout", Duration.class);

        assertEquals(Duration.ofMillis(3_000), getter.invoke(options));
        setter.invoke(options, Duration.ofMillis(17));
        assertEquals(Duration.ofMillis(17), getter.invoke(options));
        assertThrows(
            IllegalArgumentException.class,
            () -> options.setSessionRelocationSealTimeout(null));

        for (Duration invalid : new Duration[] {
            Duration.ZERO,
            Duration.ofMillis(-1),
            Duration.ofNanos(1),
            Duration.ofSeconds(Long.MAX_VALUE)}) {
            InvocationTargetException failure = assertThrows(
                InvocationTargetException.class,
                () -> setter.invoke(options, invalid));
            assertEquals(
                IllegalArgumentException.class,
                failure.getCause().getClass());
        }
    }

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
