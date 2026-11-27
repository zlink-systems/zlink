package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.spots.ZLinkRelocationSettlementFixture.Fixture;

import java.time.Instant;
import java.util.ArrayList;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;

/**
 * Host relocation flow §14: a unit still unsettled at the host stop deadline ends with {@code
 * Unavailable} for its Join caller, and after the deadline the Store is not called or retried for
 * it. The deadline timer and the retry timer are driven by hand, so no test step sleeps.
 */
final class ZLinkCanonicalRelocationStopDeadlineTest {
    @Test
    void unitWithHeldStoreReadEndsUnavailableAtTheDeadlineAndNeverCallsTheStoreAgain() {
        Fixture fixture = new Fixture(true);
        fixture.stage();
        fixture.heldReads = new CompletableFuture<>();
        var settlement = settle(fixture);
        int callsAtDeadline = fixture.storeCalls.get();

        fixture.source.stopAt(Instant.now().plusSeconds(30));
        runDeadlineTimer(fixture);

        assertUnavailable(settlement);
        fixture.heldReads.complete(null);
        assertEquals(callsAtDeadline, fixture.storeCalls.get(), "no Store call after the deadline");
        assertTrue(fixture.management.isEmpty(), "no retry is scheduled after the deadline");
    }

    @Test
    void unsettledUnitEndsUnavailableAtTheDeadlineAndItsQueuedRetryDoesNotCallTheStore() {
        Fixture fixture = new Fixture(true);
        fixture.stage();
        var settlement = settle(fixture);
        assertFalse(settlement.isDone(), "the source still owns the authority");
        int callsAtDeadline = fixture.storeCalls.get();

        fixture.source.stopAt(Instant.now().plusSeconds(30));
        var timers = new ArrayList<Runnable>();
        fixture.management.drainTo(timers);
        assertEquals(2, timers.size(), "one Store retry and one deadline timer are pending");
        timers.get(1).run();
        assertUnavailable(settlement);
        timers.get(0).run();

        assertEquals(callsAtDeadline, fixture.storeCalls.get(), "no Store call after the deadline");
        assertTrue(fixture.management.isEmpty(), "no retry is scheduled after the deadline");
    }

    @Test
    void failingStoreUnitEndsUnavailableAtTheDeadlineAndItsQueuedRetryDoesNotCallTheStore() {
        Fixture fixture = new Fixture(true);
        fixture.stage();
        fixture.failReads.set(true);
        var settlement = settle(fixture);
        assertFalse(settlement.isDone(), "a failing Store is retried, not terminal");
        int callsAtDeadline = fixture.storeCalls.get();

        fixture.source.stopAt(Instant.now().plusSeconds(30));
        var timers = new ArrayList<Runnable>();
        fixture.management.drainTo(timers);
        timers.get(1).run();
        assertUnavailable(settlement);
        timers.get(0).run();

        assertEquals(callsAtDeadline, fixture.storeCalls.get(), "no Store call after the deadline");
        assertTrue(fixture.management.isEmpty(), "no retry is scheduled after the deadline");
    }

    @Test
    void unitStartedAfterTheDeadlineEndsUnavailableWithoutAStoreCall() {
        Fixture fixture = new Fixture(true);
        fixture.stage();
        fixture.source.stopAt(Instant.now().minusSeconds(1));
        int callsAtDeadline = fixture.storeCalls.get();

        assertUnavailable(settle(fixture));

        assertEquals(callsAtDeadline, fixture.storeCalls.get(), "no Store call after the deadline");
        assertTrue(fixture.management.isEmpty(), "no retry is scheduled after the deadline");
    }

    private static CompletableFuture<?> settle(Fixture fixture) {
        return fixture.source
                .settleAuthority(
                        fixture.targetRid,
                        fixture.request.fence(),
                        Instant.now().plusSeconds(3_600))
                .toCompletableFuture();
    }

    private static void runDeadlineTimer(Fixture fixture) {
        var timers = new ArrayList<Runnable>();
        fixture.management.drainTo(timers);
        assertEquals(1, timers.size(), "only the deadline timer is pending");
        timers.get(0).run();
    }

    private static void assertUnavailable(CompletableFuture<?> settlement) {
        assertTrue(settlement.isDone(), "the Join caller is released at the deadline");
        var failure = assertThrows(ExecutionException.class, settlement::get);
        var cause = assertInstanceOf(ZLinkFrameworkException.class, failure.getCause());
        assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, cause.kind());
    }
}
