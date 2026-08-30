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

class PerfMultiRoutedSendCoordinatorTest {
    @Test
    void inlineTerminalsAdvanceOneSocketPerRoundUntilPending() {
        List<Integer> order = new ArrayList<>();
        AtomicReferenceArray<CompletableFuture<Void>> pending =
            new AtomicReferenceArray<>(3);
        int[] submissions = new int[3];
        var admissions = new PerfMultiRoutedSendCoordinator.AdmissionRoundRobin(
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
            "pending admissions must not be submitted again");

        for (int index = 0; index < 3; index++) {
            pending.get(index).complete(null);
        }
        order.clear();
        assertTrue(admissions.submitRound());
        assertEquals(List.of(0, 1, 2), order,
            "pending completion makes sockets available to the coordinator");
    }

    @Test
    void admissionOnlyModeBurstsInlineTerminalsUntilActualPending() {
        List<Integer> order = new ArrayList<>();
        int[] submissions = new int[2];
        var admissions = new PerfMultiRoutedSendCoordinator.AdmissionRoundRobin(
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
        var admissions = new PerfMultiRoutedSendCoordinator.AdmissionRoundRobin(
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
            "admission completion, not a reply, enables the next submit");
        assertEquals(List.of(coordinatorThread, coordinatorThread),
            submitThreads,
            "completion threads must never submit directly");
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
            () -> PerfMultiRoutedSendCoordinator.runAdmissions(1, activeEnd,
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
                        "socket was resubmitted while pending");
                }, Duration.ofSeconds(1), "test admission-only sends"));
        completionThread.join();

        assertNull(completionFailure.get());
        assertSame(terminalFailure, failure.getCause());
        assertEquals(2, submissions[0]);
        assertEquals(List.of(callerThread, callerThread), submitThreads,
            "completion thread must only signal the caller thread");
    }
}
