package systems.zlink.perf.multi;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicReferenceArray;
import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

class PerfMultiTargetCoordinatorTest {
    @Test
    void inlineTerminalsAdvanceOneSocketPerRoundUntilRetryWait() {
        List<Integer> order = new ArrayList<>();
        AtomicReferenceArray<CompletableFuture<Void>> pending =
            new AtomicReferenceArray<>(3);
        int[] submissions = new int[3];
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            3, Long.MAX_VALUE, index -> {
                order.add(index);
                if (++submissions[index] == 1) {
                    return CompletableFuture.completedFuture(null);
                }
                CompletableFuture<Void> stage = new CompletableFuture<>();
                pending.set(index, stage);
                return stage;
            });

        assertTrue(admissions.submitRound());
        assertEquals(List.of(0, 1, 2), order);
        assertTrue(admissions.submitRound());
        assertEquals(List.of(0, 1, 2, 1, 2, 0), order,
            "each round rotates its first socket and submits each socket once");
        assertFalse(admissions.submitRound(),
            "sends awaiting WRITABLE retry must not be submitted again");

        for (int index = 0; index < 3; index++) {
            pending.get(index).complete(null);
        }
        order.clear();
        assertTrue(admissions.submitRound());
        assertEquals(List.of(0, 1, 2), order,
            "completed retry makes sockets available to the coordinator");
    }

    @Test
    void admissionOnlyModeBurstsInlineTerminalsUntilRetryWait() {
        List<Integer> order = new ArrayList<>();
        int[] submissions = new int[2];
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            2, Long.MAX_VALUE, index -> {
                order.add(index);
                if (++submissions[index] == 1) {
                    return CompletableFuture.completedFuture(null);
                }
                return new CompletableFuture<>();
            }, false, true);

        assertTrue(admissions.submitRound());
        assertEquals(List.of(0, 0, 1, 1), order);
        assertFalse(admissions.submitRound());
    }

    @Test
    void terminalCompletionOnlyPublishesReadinessToCoordinatorThread()
        throws Exception {
        Thread coordinatorThread = Thread.currentThread();
        List<Thread> submitThreads = new ArrayList<>();
        List<CompletableFuture<Void>> stages = new ArrayList<>();
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            1, Long.MAX_VALUE, index -> {
                submitThreads.add(Thread.currentThread());
                CompletableFuture<Void> stage = new CompletableFuture<>();
                stages.add(stage);
                return stage;
            });

        assertTrue(admissions.submitRound());
        Thread completionThread = new Thread(() -> stages.get(0).complete(null),
            "test-admission-completion");
        completionThread.start();
        completionThread.join();

        assertTrue(admissions.submitRound(),
            "send completion after WRITABLE retry enables the next submit");
        assertEquals(List.of(coordinatorThread, coordinatorThread),
            submitThreads,
            "completion threads must never submit directly");
    }

    @Test
    void teardownKeepsReceivingWhileASendTerminalIsOutstanding() {
        // The echo relay holds one reply under Core admission and stops
        // pulling requests until it is admitted, so a client that blocks on
        // its own send terminals without receiving deadlocks the pair. The
        // teardown wait must keep taking receive turns (C echo client drain,
        // .NET PerfMultiEchoReplyDrain.WaitAsync).
        CompletableFuture<Void> stage = new CompletableFuture<>();
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            1, Long.MAX_VALUE, index -> stage, true, false);
        assertTrue(admissions.submitRound());

        int[] turns = {0};
        long drained = admissions.awaitLatestWhileReceiving(
            Duration.ofSeconds(5), "test teardown drain", 0L, waitMillis -> {
                if (++turns[0] == 3) {
                    stage.complete(null);
                }
                return 2;
            });

        assertEquals(3, turns[0],
            "the wait must take receive turns until the terminal arrives");
        assertEquals(6L, drained, "every consumed reply is reported");
        assertEquals(0, admissions.pendingCount());
    }

    @Test
    void teardownKeepsReceivingUntilTheOwedEchoesArrive() {
        // Every admitted request owes one echo. Leaving those echoes in the
        // client receive queue strands the relay's last reply under admission,
        // so the window must not end at the send terminal. C drains on
        // `tracker_has_retained_sends || tracker_has_pending_replies`.
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            1, Long.MAX_VALUE,
            index -> CompletableFuture.completedFuture(null), true, false);
        assertTrue(admissions.submitRound());
        assertEquals(1L, admissions.admittedCount());

        int[] turns = {0};
        long drained = admissions.awaitLatestWhileReceiving(
            Duration.ofSeconds(5), "test teardown drain", 0L,
            waitMillis -> ++turns[0] < 3 ? 0 : 1);

        assertEquals(3, turns[0],
            "the terminal alone must not end the window while an echo is owed");
        assertEquals(1L, drained);
    }

    @Test
    void teardownReceivesUntilTheBoundedWindowExpiresThenReportsTheTimeout() {
        var admissions = new PerfMultiTargetCoordinator.AdmissionRoundRobin(
            1, Long.MAX_VALUE, index -> new CompletableFuture<Void>(),
            true, false);
        assertTrue(admissions.submitRound());

        int[] turns = {0};
        IllegalStateException failure = assertThrows(IllegalStateException.class,
            () -> admissions.awaitLatestWhileReceiving(
                Duration.ofMillis(200), "test teardown drain", 0L,
                waitMillis -> {
                    turns[0]++;
                    return 0;
                }));

        assertTrue(failure.getMessage().endsWith(" timed out"),
            "the bounded window still reports the policy timeout: "
                + failure.getMessage());
        assertTrue(turns[0] > 0,
            "the window must be spent receiving, never blocking blind");
    }

    @Test
    void admissionOnlyRunWakesForCompletionAndSubmitsOnCallerThread()
        throws Exception {
        Thread callerThread = Thread.currentThread();
        List<Thread> submitThreads = new ArrayList<>();
        CompletableFuture<Void> first = new CompletableFuture<>();
        CountDownLatch firstSubmitted = new CountDownLatch(1);
        AtomicReference<Throwable> completionFailure = new AtomicReference<>();
        long activeEnd = System.nanoTime()
            + TimeUnit.SECONDS.toNanos(1);
        IllegalArgumentException terminalFailure =
            new IllegalArgumentException("stop test admission loop");

        Thread completionThread = new Thread(() -> {
            try {
                assertTrue(firstSubmitted.await(1, TimeUnit.SECONDS));
                first.complete(null);
            } catch (Throwable error) {
                completionFailure.set(error);
                first.completeExceptionally(error);
            }
        }, "test-admission-only-completion");
        completionThread.start();

        int[] submissions = {0};
        IllegalStateException failure = assertThrows(IllegalStateException.class,
            () -> PerfMultiTargetCoordinator.runAdmissions(1, activeEnd,
                index -> {
                    submitThreads.add(Thread.currentThread());
                    submissions[0]++;
                    if (submissions[0] == 1) {
                        firstSubmitted.countDown();
                        return first;
                    }
                    if (submissions[0] == 2) {
                        return CompletableFuture.failedFuture(terminalFailure);
                    }
                    throw new AssertionError(
                        "socket was resubmitted while awaiting WRITABLE retry");
                }, Duration.ofSeconds(1), "test admission-only sends"));
        completionThread.join();

        assertNull(completionFailure.get());
        assertSame(terminalFailure, failure.getCause());
        assertEquals(2, submissions[0]);
        assertEquals(List.of(callerThread, callerThread), submitThreads,
            "completion thread must only signal the caller thread");
    }
}
