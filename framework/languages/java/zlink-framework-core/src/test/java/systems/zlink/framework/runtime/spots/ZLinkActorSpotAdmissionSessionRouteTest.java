package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;

final class ZLinkActorSpotAdmissionSessionRouteTest {
    @Test
    void readyDoesNotWaitButCommand44WaitsForCompletedEvidence()
        throws InterruptedException {
        CompletableFuture<Void> completedEvidence = new CompletableFuture<>();
        AtomicBoolean ready = new AtomicBoolean();
        AtomicInteger command44 = new AtomicInteger();
        CountDownLatch sent = new CountDownLatch(1);
        List<Throwable> failures = new CopyOnWriteArrayList<>();
        var handoff = new ZLinkActorSpotAdmission.SourceCleanupRouteHandoff(
            () -> completedEvidence,
            () -> {
                command44.incrementAndGet();
                sent.countDown();
            },
            () -> false,
            failures::add,
            Duration.ofMillis(1));

        handoff.start();
        ready.set(true);

        assertTrue(ready.get(),
            "target Ready and callbacks are independent of source cleanup");
        assertEquals(0, command44.get(),
            "command 44 is absent before exact Completed evidence");

        completedEvidence.complete(null);
        assertTrue(sent.await(1, TimeUnit.SECONDS));
        assertEquals(1, command44.get());
        assertTrue(failures.isEmpty());
    }

    @Test
    void transientCompletedReadFailureRetainsTheExactHandoff()
        throws InterruptedException {
        AtomicInteger reads = new AtomicInteger();
        AtomicInteger command44 = new AtomicInteger();
        CountDownLatch retried = new CountDownLatch(1);
        CompletableFuture<Void> recoveredEvidence = new CompletableFuture<>();
        var handoff = new ZLinkActorSpotAdmission.SourceCleanupRouteHandoff(
            () -> {
                if (reads.incrementAndGet() == 1) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException("authority read unavailable"));
                }
                retried.countDown();
                return recoveredEvidence;
            },
            command44::incrementAndGet,
            () -> false,
            failure -> { },
            Duration.ofMillis(1));

        handoff.start();

        assertTrue(retried.await(1, TimeUnit.SECONDS));
        assertEquals(0, command44.get());
        recoveredEvidence.complete(null);
        awaitCount(command44, 1);
        assertEquals(2, reads.get());
        assertEquals(1, command44.get());
    }

    @Test
    void shutdownTerminatesTheDelayedEvidenceRetry() throws InterruptedException {
        AtomicInteger reads = new AtomicInteger();
        AtomicInteger command44 = new AtomicInteger();
        AtomicBoolean stopped = new AtomicBoolean();
        CountDownLatch terminal = new CountDownLatch(1);
        var handoff = new ZLinkActorSpotAdmission.SourceCleanupRouteHandoff(
            () -> {
                reads.incrementAndGet();
                return CompletableFuture.failedFuture(
                    new IllegalStateException("authority read unavailable"));
            },
            command44::incrementAndGet,
            stopped::get,
            failure -> terminal.countDown(),
            Duration.ofMillis(5));

        handoff.start();
        stopped.set(true);

        assertTrue(terminal.await(1, TimeUnit.SECONDS));
        int terminalReads = reads.get();
        Thread.sleep(25);
        assertEquals(terminalReads, reads.get(),
            "shutdown leaves no delayed evidence retry retaining the handoff");
        assertEquals(0, command44.get());
    }

    private static void awaitCount(AtomicInteger value, int expected)
        throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
        while (value.get() != expected && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
    }
}
