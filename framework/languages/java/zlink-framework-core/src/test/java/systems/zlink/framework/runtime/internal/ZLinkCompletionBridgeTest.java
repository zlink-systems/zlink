package systems.zlink.framework.runtime.internal;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.lang.reflect.Field;
import java.util.ArrayDeque;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.Set;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BiConsumer;

final class ZLinkCompletionBridgeTest {
    @Test
    void cancellationUsesTheDeliveredTerminalWithoutRereadingCallerState() {
        for (int terminal = 0; terminal < 4; terminal++) {
            var reads = new AtomicInteger();
            var caller =
                    new CompletableFuture<String>() {
                        @Override
                        public boolean isCancelled() {
                            reads.incrementAndGet();
                            return super.isCancelled();
                        }
                    };
            var waiter = new CompletableFuture<String>();
            ZLinkCompletionBridge.forwardCancellation(caller, waiter);
            if (terminal == 0) caller.complete("reply");
            else if (terminal == 1) caller.completeExceptionally(new IllegalStateException());
            else if (terminal == 2) caller.completeExceptionally(new CancellationException());
            else caller.completeExceptionally(new CompletionException(new CancellationException()));
            assertEquals(terminal == 2, waiter.isCancelled());
            assertEquals(0, reads.get(), "the callback already receives the terminal failure");
        }
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

    private static final class CountFuture<T> extends CompletableFuture<T> {
        int observers, futures;
        BiConsumer<? super T, ? super Throwable> callback;

        @Override
        public CompletableFuture<T> whenComplete(BiConsumer<? super T, ? super Throwable> f) {
            observers++;
            callback = f;
            return super.whenComplete(f);
        }

        @Override
        public <U> CompletableFuture<U> newIncompleteFuture() {
            futures++;
            return new CompletableFuture<U>();
        }
    }

    private static int closures(Object o, Set<Object> seen) throws Exception {
        if (o == null || !o.getClass().isHidden() || !seen.add(o)) return 0;
        if (o.getClass().getDeclaredFields().length == 0)
            return 0; // Shared noncapturing actions are not per-call closures.
        int n = 1;
        for (Field f : o.getClass().getDeclaredFields()) {
            f.setAccessible(true);
            n += closures(f.get(o), seen);
        }
        return n;
    }

    @Test
    void preservesCancellationAndDiscardBehavior() {
        for (boolean before : new boolean[] {true, false})
            for (int variant = 0; variant < 3; variant++) {
                var c = new CompletableFuture<String>();
                var a = new CompletableFuture<String>();
                var b = new CompletableFuture<String>();
                var owner = new ArrayDeque<Runnable>();
                if (before) c.cancel(false);
                if (variant == 0) ZLinkCompletionBridge.forwardCancellation(c, a);
                if (variant == 1) ZLinkCompletionBridge.forwardCancellation(c, a, b);
                if (variant == 2)
                    ZLinkCompletionBridge.forwardCancellation(
                            c, () -> owner.add(() -> a.cancel(false)));
                if (!before) c.cancel(false);
                c.cancel(false); // Repeated cancellation must not publish another owner action.
                if (variant == 2) {
                    assertTrue(!a.isDone(), "owner must execute cancellation");
                    assertTrue(owner.size() == 1, "owner once");
                    owner.remove().run();
                }
                assertTrue(a.isCancelled(), "cancellation forwarded");
                if (variant == 1) {
                    assertTrue(b.isCancelled(), "second waiter");
                }
            }
        for (int terminal = 0; terminal < 3; terminal++) {
            var c = new CompletableFuture<String>();
            var a = new CompletableFuture<String>();
            if (terminal == 0) c.complete("ok");
            else if (terminal == 1) c.completeExceptionally(new IllegalStateException());
            else c.completeExceptionally(new CompletionException(new CancellationException()));
            ZLinkCompletionBridge.forwardCancellation(c, a);
            assertTrue(!a.isDone(), "non-cancel failure/normal not forwarded");
        }
        var cancelled = new CompletableFuture<String>();
        var target = new CompletableFuture<String>();
        cancelled.completeExceptionally(new CancellationException());
        ZLinkCompletionBridge.forwardCancellation(cancelled, target);
        assertTrue(target.isCancelled(), "direct exceptional cancellation forwarded");
        var c = new CompletableFuture<String>();
        var a = new CompletableFuture<String>();
        a.complete("won");
        ZLinkCompletionBridge.forwardCancellation(c, a, (CompletionStage<?>) null);
        c.cancel(false);
        assertTrue(a.join().equals("won"), "completed waiter preserved");
        var d = new CompletableFuture<String>();
        int[] discard = {0};
        ZLinkCompletionBridge.completeOrDiscard(d, "accepted", v -> discard[0]++);
        assertTrue(!d.cancel(false), "late cancellation rejected");
        assertTrue(discard[0] == 0, "accepted not discarded");
        ZLinkCompletionBridge.completeOrDiscard(d, "late", v -> discard[0]++);
        assertTrue(discard[0] == 1, "late discarded once");
        var e = new CompletableFuture<String>();
        e.cancel(false);
        ZLinkCompletionBridge.completeOrDiscard(e, "cancelled", v -> discard[0]++);
        assertTrue(discard[0] == 2, "cancelled result discarded");
        // Caller isolation: cancellation of one bridge does not cancel an unrelated bridge.
        var x = new CompletableFuture<String>();
        var y = new CompletableFuture<String>();
        var wx = new CompletableFuture<String>();
        var wy = new CompletableFuture<String>();
        ZLinkCompletionBridge.forwardCancellation(x, wx);
        ZLinkCompletionBridge.forwardCancellation(y, wy);
        x.cancel(false);
        assertTrue(wx.isCancelled() && !wy.isDone() && !y.isDone(), "isolation");
        y.complete("ok");
        // A forwarding exception is captured by the ignored dependent, not thrown by cancel().
        var f = new CompletableFuture<String>();
        ZLinkCompletionBridge.forwardCancellation(
                f,
                () -> {
                    throw new IllegalStateException("forward");
                });
        assertTrue(f.cancel(false), "forward exception");
    }

    @Test
    void singleAndPairUseOneCapturingClosurePerCall() throws Exception {
        final int registrations = 1000;
        for (boolean completed : new boolean[] {false, true}) {
            for (boolean pair : new boolean[] {false, true}) {
                int closureCount = 0;
                int futureCount = 0;
                for (int index = 0; index < registrations; index++) {
                    var caller = new CountFuture<String>();
                    var waiter = new CompletableFuture<String>();
                    if (completed) caller.complete("done");
                    if (pair)
                        ZLinkCompletionBridge.forwardCancellation(
                                caller, waiter, new CompletableFuture<String>());
                    else ZLinkCompletionBridge.forwardCancellation(caller, waiter);
                    assertEquals(1, caller.observers);
                    assertEquals(completed ? 0 : 1, caller.getNumberOfDependents());
                    closureCount +=
                            closures(
                                    caller.callback,
                                    Collections.newSetFromMap(new IdentityHashMap<>()));
                    futureCount += caller.futures;
                    caller.complete("done");
                    assertFalse(waiter.isDone());
                }
                assertEquals(registrations, closureCount);
                assertEquals(registrations, futureCount);
            }
        }
    }

    @Test
    void waiterConversionStaysDeferredUntilCancellation() {
        var conversions = new AtomicInteger();
        var caller = new CompletableFuture<String>();
        var waiter =
                new CompletableFuture<String>() {
                    @Override
                    public CompletableFuture<String> toCompletableFuture() {
                        conversions.incrementAndGet();
                        return this;
                    }
                };
        ZLinkCompletionBridge.forwardCancellation(caller, waiter);
        assertEquals(0, conversions.get());
        caller.cancel(false);
        assertEquals(1, conversions.get());
        assertTrue(waiter.isCancelled());
    }
}
