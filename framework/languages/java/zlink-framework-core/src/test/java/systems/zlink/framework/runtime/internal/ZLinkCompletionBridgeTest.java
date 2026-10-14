package systems.zlink.framework.runtime.internal;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkCompletionBridgeTest {
    @Test
    void dispatchedSourceCommitsImmediatelyAndKeepsOwnerObservationOnItsTurn() throws Exception {
        var source = ZLinkCompletionBridge.<String>completionSource();
        var owner = Thread.currentThread();
        var observed = new AtomicReference<Thread>();
        ZLinkCompletionBridge.whenCompleteOwner(
                source, (value, failure) -> observed.set(Thread.currentThread()));
        var continuation =
                source.thenApply(
                        value -> {
                            assertNotSame(owner, Thread.currentThread());
                            return value;
                        });
        assertTrue(source.complete("accepted"));
        assertTrue(source.isDone());
        assertSame(owner, observed.get());
        assertEquals("accepted", continuation.get(2, java.util.concurrent.TimeUnit.SECONDS));
    }

    @Test
    void failurePropagationKeepsDependentRecoveryOutsideTheOwnerTurn() throws Exception {
        var source = ZLinkCompletionBridge.<String>completionSource();
        var owner = Thread.currentThread();
        var failure = new IllegalStateException("owner rejected");
        var recovered =
                source.toCompletableFuture()
                        .thenApply(value -> value)
                        .exceptionally(
                                error -> {
                                    assertNotSame(owner, Thread.currentThread());
                                    assertSame(failure, error.getCause());
                                    return "rejected";
                                });
        assertTrue(source.completeExceptionally(failure));
        assertEquals("rejected", recovered.get(2, java.util.concurrent.TimeUnit.SECONDS));
    }

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
