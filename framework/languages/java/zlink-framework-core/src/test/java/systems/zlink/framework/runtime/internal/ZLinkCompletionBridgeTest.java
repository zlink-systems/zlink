package systems.zlink.framework.runtime.internal;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkCompletionBridgeTest {
    @Test
    void cancellationReachesEachPendingWaiter() {
        var caller = new CompletableFuture<String>();
        var admission = new CompletableFuture<Void>();
        var reply = new CompletableFuture<String>();
        ZLinkCompletionBridge.forwardCancellation(caller, admission, reply);

        assertTrue(caller.cancel(false));
        assertTrue(admission.isCancelled());
        assertTrue(reply.isCancelled());
    }

    @Test
    void normalCompletionKeepsWaiterActive() {
        var caller = new CompletableFuture<String>();
        var waiter = new CompletableFuture<String>();
        ZLinkCompletionBridge.forwardCancellation(caller, waiter);

        assertTrue(caller.complete("done"));
        assertFalse(waiter.isCancelled());
    }

    @Test
    void cancellationRunsForwardingAction() {
        var caller = new CompletableFuture<String>();
        var forwarded = new AtomicInteger();
        ZLinkCompletionBridge.forwardCancellation(caller, forwarded::incrementAndGet);

        assertTrue(caller.cancel(false));
        assertEquals(1, forwarded.get());
    }

    @Test
    void lateResultIsDiscardedAfterCancellation() {
        var caller = new CompletableFuture<String>();
        var discarded = new AtomicReference<String>();
        assertTrue(caller.cancel(false));

        ZLinkCompletionBridge.completeOrDiscard(caller, "late", discarded::set);

        assertEquals("late", discarded.get());
    }
}
