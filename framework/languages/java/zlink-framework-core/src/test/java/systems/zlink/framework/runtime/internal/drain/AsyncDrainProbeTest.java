package systems.zlink.framework.runtime.internal.drain;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;

final class AsyncDrainProbeTest {
    @Test
    void namesIncompleteWorkWithoutChangingObservedFailure() {
        var probe = new AsyncDrainProbe();
        var observed = new CompletableFuture<Void>();
        probe.expect("relay:actor:7", "Actor source");
        probe.completeOn(observed, "relay:actor:7");

        AssertionError pending = assertThrows(AssertionError.class, probe::assertDrained);
        assertTrue(pending.getMessage().contains("relay:actor:7"));
        assertTrue(pending.getMessage().contains("Actor source"));
        assertFalse(observed.isDone());

        var failure = new IllegalStateException("relay failed");
        observed.completeExceptionally(failure);
        probe.assertDrained();
        assertSame(failure, assertThrows(Exception.class, observed::join).getCause());
    }

    @Test
    void failedFirstRelayLeavesUnstartedRecordPending() {
        var probe = new AsyncDrainProbe();
        var first = probe.expect("relay:actor:1", "Actor source");
        probe.expect("relay:actor:2", "Actor source");
        var failed =
                CompletableFuture.<Void>failedFuture(new IllegalStateException("relay failed"));
        probe.completeOn(failed, first);

        assertEquals(
                java.util.List.of(new AsyncDrainProbe.Pending("relay:actor:2", "Actor source")),
                probe.pending());
        assertThrows(AssertionError.class, probe::assertDrained);
    }

    @Test
    void repeatedRelayNameRetainsEachObligation() {
        var probe = new AsyncDrainProbe();
        var first = probe.expect("relay:actor:1", "Actor source");
        var second = probe.expect("relay:actor:1", "Actor source");
        probe.completeOn(CompletableFuture.completedFuture(null), second);

        assertEquals(1, probe.pending().size());
        assertThrows(AssertionError.class, probe::assertDrained);
        probe.completeOn(CompletableFuture.completedFuture(null), first);
        probe.assertDrained();
    }
}
