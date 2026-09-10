package systems.zlink.perf.multi;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.atomic.AtomicReferenceArray;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.SendSubmission;
import systems.zlink.contracts.sockets.SubmitResult;

class PerfMultiRoutedSendCoordinatorTest {
    @Test
    void okImmediatelyKeepsSocketAvailableWithoutReadingAdmissionStage() {
        List<Integer> order = new ArrayList<>();
        AtomicReferenceArray<CompletableFuture<Void>> pending =
            new AtomicReferenceArray<>(3);
        AtomicInteger admittedReads = new AtomicInteger();
        int[] submissions = new int[3];
        var coordinator = coordinator(3, index -> {
            order.add(index);
            if (++submissions[index] == 1) {
                return okSubmission(admittedReads);
            }
            CompletableFuture<Void> stage = new CompletableFuture<>();
            pending.set(index, stage);
            return backpressuredSubmission(stage);
        });

        assertTrue(coordinator.submitRound());
        assertEquals(List.of(0, 1, 2), order);
        assertEquals(0, admittedReads.get(),
            "OK must not inspect or await admitted()");
        assertTrue(coordinator.submitRound());
        assertEquals(List.of(0, 1, 2, 1, 2, 0), order,
            "OK sockets remain immediately eligible in rotating order");
        assertFalse(coordinator.submitRound(),
            "only BACKPRESSURED sockets become unavailable");

        for (int index = 0; index < 3; index++) {
            pending.get(index).complete(null);
        }
        order.clear();
        assertTrue(coordinator.submitRound());
        assertEquals(List.of(0, 1, 2), order);
    }

    @Test
    void admissionCompletionOnlyPublishesReadinessToCoordinatorThread()
        throws Exception {
        Thread callerThread = Thread.currentThread();
        List<Thread> submitThreads = new ArrayList<>();
        List<CompletableFuture<Void>> stages = new ArrayList<>();
        var coordinator = coordinator(1, index -> {
            submitThreads.add(Thread.currentThread());
            CompletableFuture<Void> stage = new CompletableFuture<>();
            stages.add(stage);
            return backpressuredSubmission(stage);
        });

        assertTrue(coordinator.submitRound());
        Thread completionThread = new Thread(() -> stages.get(0).complete(null),
            "test-admission-completion");
        completionThread.start();
        completionThread.join();

        assertTrue(coordinator.submitRound());
        assertEquals(List.of(callerThread, callerThread), submitThreads,
            "completion threads must never submit directly");
    }

    @Test
    void backpressureOnOneSocketDoesNotStopOtherSockets() {
        CompletableFuture<Void> blocked = new CompletableFuture<>();
        AtomicInteger admittedReads = new AtomicInteger();
        int[] submissions = new int[2];
        var coordinator = coordinator(2, index -> {
            submissions[index]++;
            return index == 0
                ? backpressuredSubmission(blocked)
                : okSubmission(admittedReads);
        });

        assertTrue(coordinator.submitRound());
        assertTrue(coordinator.submitRound());
        assertEquals(1, submissions[0]);
        assertEquals(2, submissions[1],
            "an OK socket must continue while its peer waits for admission");
        assertEquals(0, admittedReads.get());

        blocked.complete(null);
        assertTrue(coordinator.submitRound());
        assertEquals(2, submissions[0]);
    }

    @Test
    void teardownKeepsReceivingWhileBackpressuredAdmissionIsOutstanding() {
        CompletableFuture<Void> stage = new CompletableFuture<>();
        var coordinator = coordinator(1,
            index -> backpressuredSubmission(stage));
        assertTrue(coordinator.submitRound());

        int[] turns = {0};
        long drained = coordinator.awaitPendingWhileReceiving(
            Duration.ofSeconds(5), "test teardown drain", 0L, waitMillis -> {
                if (++turns[0] == 3) {
                    stage.complete(null);
                }
                return 2;
            });

        assertEquals(3, turns[0]);
        assertEquals(6L, drained);
        assertEquals(0, coordinator.pendingCount());
    }

    @Test
    void teardownKeepsReceivingUntilOwedEchoArrivesAfterOk() {
        var coordinator = coordinator(1,
            index -> okSubmission(new AtomicInteger()));
        assertTrue(coordinator.submitRound());
        assertEquals(1L, coordinator.admittedCount());

        int[] turns = {0};
        long drained = coordinator.awaitPendingWhileReceiving(
            Duration.ofSeconds(5), "test teardown drain", 0L,
            waitMillis -> ++turns[0] < 3 ? 0 : 1);

        assertEquals(3, turns[0]);
        assertEquals(1L, drained);
    }

    @Test
    void teardownReportsTimeoutForOutstandingBackpressure() {
        var coordinator = coordinator(1,
            index -> backpressuredSubmission(new CompletableFuture<>()));
        assertTrue(coordinator.submitRound());

        int[] turns = {0};
        IllegalStateException failure = assertThrows(IllegalStateException.class,
            () -> coordinator.awaitPendingWhileReceiving(
                Duration.ofMillis(200), "test teardown drain", 0L,
                waitMillis -> {
                    turns[0]++;
                    return 0;
                }));

        assertTrue(failure.getMessage().endsWith(" timed out"));
        assertTrue(turns[0] > 0);
    }

    @Test
    void admissionOnlyRunWakesAndResubmitsOnCallerThread() throws Exception {
        Thread callerThread = Thread.currentThread();
        List<Thread> submitThreads = new ArrayList<>();
        CompletableFuture<Void> first = new CompletableFuture<>();
        CountDownLatch firstSubmitted = new CountDownLatch(1);
        AtomicReference<Throwable> completionFailure = new AtomicReference<>();
        long activeEnd = System.nanoTime() + TimeUnit.SECONDS.toNanos(1);
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
                        return backpressuredSubmission(first);
                    }
                    throw terminalFailure;
                }, Duration.ofSeconds(1), "test admission-only sends"));
        completionThread.join();

        assertNull(completionFailure.get());
        assertSame(terminalFailure, failure.getCause());
        assertEquals(2, submissions[0]);
        assertEquals(List.of(callerThread, callerThread), submitThreads);
    }

    private static PerfMultiRoutedSendCoordinator.BackpressureCoordinator
            coordinator(int socketCount,
                        PerfMultiRoutedSendCoordinator.Submitter submitter) {
        return new PerfMultiRoutedSendCoordinator.BackpressureCoordinator(
            socketCount, Long.MAX_VALUE, submitter);
    }

    private static SendSubmission okSubmission(AtomicInteger admittedReads) {
        return new SendSubmission() {
            @Override
            public SubmitResult result() {
                return SubmitResult.OK;
            }

            @Override
            public CompletionStage<Void> admitted() {
                admittedReads.incrementAndGet();
                throw new AssertionError("OK admission stage was inspected");
            }
        };
    }

    private static SendSubmission backpressuredSubmission(
            CompletionStage<Void> admitted) {
        return new SendSubmission() {
            @Override
            public SubmitResult result() {
                return SubmitResult.BACKPRESSURED;
            }

            @Override
            public CompletionStage<Void> admitted() {
                return admitted;
            }
        };
    }
}
