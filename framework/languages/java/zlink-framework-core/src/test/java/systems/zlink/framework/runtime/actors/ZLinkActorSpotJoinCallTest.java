package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
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

    @Test
    void lookupFaultRetainsRecoveryUntilExactCommittedAuthorityAppears()
        throws InterruptedException {
        AtomicInteger lookups = new AtomicInteger();
        AtomicInteger aborts = new AtomicInteger();
        CountDownLatch retried = new CountDownLatch(1);
        CompletableFuture<Optional<String>> committed =
            new CompletableFuture<>();
        var recovery = new ZLinkActorSpotJoinCall.CommittedLookupRecovery<>(
            () -> {
                if (lookups.incrementAndGet() == 1) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("Location read unavailable"));
                }
                retried.countDown();
                return committed;
            },
            Duration.ofMillis(1),
            () -> false);

        CompletableFuture<Optional<String>> result =
            recovery.start().toCompletableFuture();
        result.thenAccept(authority -> {
            if (authority.isEmpty()) {
                aborts.incrementAndGet();
            }
        });

        assertTrue(retried.await(1, TimeUnit.SECONDS));
        assertFalse(result.isDone(),
            "a lookup failure is unknown, not proof that target commit is absent");
        assertEquals(0, aborts.get());

        committed.complete(Optional.of("target-authority"));
        assertEquals("target-authority", result.join().orElseThrow());
        assertEquals(2, lookups.get());
        assertEquals(0, aborts.get(),
            "later committed evidence converges target and never resumes source");
    }

    @Test
    void shutdownSettlesAPendingLookupAndStopsDelayedRetries()
        throws InterruptedException {
        AtomicInteger lookups = new AtomicInteger();
        java.util.concurrent.atomic.AtomicBoolean stopped =
            new java.util.concurrent.atomic.AtomicBoolean();
        CompletableFuture<Optional<String>> unavailable =
            new CompletableFuture<>();
        var recovery = new ZLinkActorSpotJoinCall.CommittedLookupRecovery<>(
            () -> {
                lookups.incrementAndGet();
                return unavailable;
            },
            Duration.ofMillis(2),
            stopped::get);

        CompletableFuture<Optional<String>> result =
            recovery.start().toCompletableFuture();
        stopped.set(true);

        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (!result.isDone() && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(result.isCompletedExceptionally());
        int terminalLookups = lookups.get();
        Thread.sleep(20);
        assertEquals(terminalLookups, lookups.get(),
            "shutdown leaves no delayed lookup retry retaining the recovery");
    }
}
