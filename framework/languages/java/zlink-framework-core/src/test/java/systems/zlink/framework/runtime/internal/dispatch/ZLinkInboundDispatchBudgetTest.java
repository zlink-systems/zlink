package systems.zlink.framework.runtime.internal.dispatch;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkApplicationHwmProfile;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.configuration.ZLinkInboundDispatchRegistration;

final class ZLinkInboundDispatchBudgetTest {
    @Test
    void keepsHandlerBytesReservedUntilTerminalCompletion() {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(10);
        AtomicInteger wakeups = new AtomicInteger();
        budget.onCapacityAvailable(wakeups::incrementAndGet);

        ZLinkInboundDispatchBudget.Lease lease = budget.track(10);
        assertFalse(budget.canStartApplicationReceive());
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(10, 10, 10, 0, true),
            budget.snapshot());

        lease.handlerStarted();
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(10, 10, 0, 10, true),
            budget.snapshot());
        assertFalse(budget.canStartApplicationReceive());
        assertEquals(0, wakeups.get());

        lease.close();
        lease.close();
        assertTrue(budget.canStartApplicationReceive());
        assertEquals(1, wakeups.get());
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(10, 0, 0, 0, false),
            budget.snapshot());
    }

    @Test
    void cancellationOrAdmissionFailureReleasesQueuedBytes() {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(8);
        ZLinkInboundDispatchBudget.Lease lease = budget.track(8);

        assertTrue(budget.snapshot().applicationReceivePaused());
        lease.close();

        assertEquals(0, budget.snapshot().pendingPayloadBytes());
        assertFalse(budget.snapshot().applicationReceivePaused());
    }

    @Test
    void allowsAnOversizedMessageThatStartedWithAnEmptyBudget() {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(10);

        assertTrue(budget.canStartApplicationReceive());
        ZLinkInboundDispatchBudget.Lease lease = budget.track(12);

        assertFalse(budget.canStartApplicationReceive());
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(10, 12, 12, 0, true),
            budget.snapshot());

        lease.close();
        assertTrue(budget.canStartApplicationReceive());
    }

    @Test
    void admitsOnePreciseOvershootThenRejectsTheNextPayload() {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(10);

        ZLinkInboundDispatchBudget.Lease first = budget.tryTrack(8);
        ZLinkInboundDispatchBudget.Lease overshoot = budget.tryTrack(5);

        assertTrue(first != null);
        assertTrue(overshoot != null);
        assertEquals(13, budget.snapshot().pendingPayloadBytes());
        assertTrue(budget.snapshot().applicationReceivePaused());
        assertNull(budget.tryTrack(1));

        overshoot.close();
        first.close();
        assertEquals(0, budget.snapshot().pendingPayloadBytes());
        assertTrue(budget.canStartApplicationReceive());
    }

    @Test
    void atomicAdmissionRejectsAfterTheThresholdIsReached() throws Exception {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(10);
        ZLinkInboundDispatchBudget.Lease initial = budget.tryTrack(10);
        assertTrue(initial != null);
        CountDownLatch start = new CountDownLatch(1);
        ExecutorService executor = Executors.newFixedThreadPool(2);
        try {
            var first = executor.submit(() -> {
                start.await();
                return budget.tryTrack(7);
            });
            var second = executor.submit(() -> {
                start.await();
                return budget.tryTrack(7);
            });
            start.countDown();

            var firstLease = first.get();
            var secondLease = second.get();
            assertNull(firstLease);
            assertNull(secondLease);
            assertEquals(10, budget.snapshot().pendingPayloadBytes());
            if (firstLease != null) {
                firstLease.close();
            }
            if (secondLease != null) {
                secondLease.close();
            }
        } finally {
            initial.close();
            executor.shutdownNow();
        }
    }

    @Test
    void automaticSizingUsesConfiguredProcessMemoryAndProfile() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setProcessMemoryLimitBytes(1_000);
        options.setApplicationHwmProfile(ZLinkApplicationHwmProfile.THROUGHPUT);

        assertEquals(
            200,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(options));
    }

    @org.junit.jupiter.params.ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "COMPACT,20",
        "LOW_LATENCY,50",
        "BALANCED,100",
        "THROUGHPUT,200"
    })
    void automaticSizingUsesTheContractProfileRatios(
        ZLinkApplicationHwmProfile profile,
        long expected) {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setApplicationHwmProfile(profile);

        assertEquals(
            expected,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    1_000,
                    Long.MAX_VALUE,
                    9_000)));
    }

    @Test
    void explicitProcessMemoryLimitWinsOverDetectedCandidates() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setProcessMemoryLimitBytes(2_000);

        assertEquals(
            200,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    100,
                    50,
                    10_000)));
    }

    @Test
    void automaticSizingUsesTheSmallerOsAndManagedHeapLimit() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertEquals(
            76,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    1_024,
                    768,
                    16_384)));
    }

    @Test
    void automaticSizingUsesManagedHeapWhenOsLimitIsUnavailable() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertEquals(
            76,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    0,
                    768,
                    16_384)));
    }

    @Test
    void automaticSizingUsesOsLimitWhenManagedHeapIsUnbounded() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertEquals(
            102,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    1_024,
                    Long.MAX_VALUE,
                    16_384)));
    }

    @Test
    void automaticSizingFallsBackToPhysicalMemoryWhenNoFiniteLimitExists() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertEquals(
            100,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    0,
                    Long.MAX_VALUE,
                    1_000)));
    }

    @Test
    void cgroupUnlimitedSentinelIsNotTreatedAsAnEffectiveLimit() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertEquals(
            200,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(
                    0x7FFF_FFFF_FFFF_0000L,
                    2_000,
                    16_384)));
    }

    @Test
    void zeroApplicationHwmIsExplicitlyUnlimited() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setApplicationHwmBytes(0);

        assertEquals(
            0,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(1, 1, 1)));
    }

    @Test
    void automaticSizingRejectsAZeroResult() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setApplicationHwmProfile(ZLinkApplicationHwmProfile.COMPACT);

        assertThrows(
            ZLinkConfigurationException.class,
            () -> ZLinkInboundDispatchBudget.resolveApplicationHwm(
                options,
                new ZLinkInboundDispatchBudget.ProcessMemoryCandidates(1, 1, 1)));
    }

    @Test
    void explicitNegativeApplicationHwmIsAConfigurationError() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();

        assertThrows(
            ZLinkConfigurationException.class,
            () -> options.setApplicationHwmBytes(-1));
    }

    @Test
    void automaticSizingUsesManagedHeapAsAnAutomaticMemoryCandidate() {
        ZLinkInboundDispatchRegistration options =
            new ZLinkInboundDispatchRegistration();
        options.setApplicationHwmProfile(ZLinkApplicationHwmProfile.BALANCED);

        long resolved = ZLinkInboundDispatchBudget.resolveApplicationHwm(options);

        assertTrue(resolved > 0);
        long managedHeapLimit = Runtime.getRuntime().maxMemory();
        assertTrue(resolved <= managedHeapLimit / 10L + 1L);
        assertEquals(
            resolved,
            ZLinkInboundDispatchBudget.resolveApplicationHwm(options));
    }

    @Test
    void completionPermitsRemainPendingUntilTheReplyIsAccepted() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(0, 1);

        ZLinkInboundDispatchBudget.CompletionPermit first =
            budget.acquireCompletionPermit().toCompletableFuture().join();
        CompletionStage<ZLinkInboundDispatchBudget.CompletionPermit> second =
            budget.acquireCompletionPermit();

        assertEquals(2, budget.pendingCompletionSends());
        assertEquals(1, budget.completionSendLimit());
        assertFalse(second.toCompletableFuture().isDone());

        first.close();
        ZLinkInboundDispatchBudget.CompletionPermit promoted =
            second.toCompletableFuture().join();
        assertEquals(1, budget.pendingCompletionSends());

        promoted.close();
        assertEquals(0, budget.pendingCompletionSends());
    }

    @Test
    void cancellingPendingCompletionPermitDoesNotStarveTheNextWaiter() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(0, 1);
        ZLinkInboundDispatchBudget.CompletionPermit active =
            budget.acquireCompletionPermit().toCompletableFuture().join();
        var cancelled = budget.acquireCompletionPermit().toCompletableFuture();
        var next = budget.acquireCompletionPermit().toCompletableFuture();

        assertTrue(cancelled.cancel(false));
        assertEquals(2, budget.pendingCompletionSends());

        active.close();
        ZLinkInboundDispatchBudget.CompletionPermit promoted = next.join();
        assertEquals(1, budget.pendingCompletionSends());

        promoted.close();
        assertEquals(0, budget.pendingCompletionSends());
    }

    @Test
    void closingCompletionAdmissionFailsPendingAndRejectsNewPermits() {
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(0, 1);
        ZLinkInboundDispatchBudget.CompletionPermit active =
            budget.acquireCompletionPermit().toCompletableFuture().join();
        CompletionStage<ZLinkInboundDispatchBudget.CompletionPermit> pending =
            budget.acquireCompletionPermit();

        budget.close();

        assertThrows(CompletionException.class,
            () -> pending.toCompletableFuture().join());
        assertThrows(CompletionException.class,
            () -> budget.acquireCompletionPermit().toCompletableFuture().join());
        active.close();
        assertEquals(0, budget.pendingCompletionSends());
    }

    @Test
    void shutdownClosesExistingPayloadLeasesWithoutReopeningReceive() {
        ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(8);
        ZLinkInboundDispatchBudget.Lease lease = budget.track(8);
        lease.handlerStarted();

        budget.close();

        assertFalse(budget.canStartApplicationReceive());
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(8, 8, 0, 8, true),
            budget.snapshot());
        assertDoesNotThrow(lease::close);
        assertDoesNotThrow(lease::close);
        assertEquals(
            new ZLinkInboundDispatchBudget.Snapshot(8, 0, 0, 0, true),
            budget.snapshot());
        assertNull(budget.tryTrack(1));
        assertThrows(
            IllegalStateException.class,
            () -> budget.track(1));
    }
}
