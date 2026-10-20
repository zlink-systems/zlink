package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.*;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkRelocationHandOffTest {
    @Test
    void sourcePreserveSettlesRetainedWorkWhileRelayAdmissionIsPending() {
        var relayAdmission = new CompletableFuture<Void>();
        var settleCalls = new AtomicInteger();
        var result =
                ZLinkRelocationHandOff.run(
                        client(settleCalls),
                        request(),
                        () -> relayAdmission,
                        Duration.ofSeconds(1),
                        Instant.now());

        assertEquals(1, settleCalls.get());
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                result.toCompletableFuture().getNow(null));
        assertTrue(relayAdmission.isCancelled());
    }

    @Test
    void sourcePreserveSettlesRetainedWorkWhenRelayAdmissionThrows() {
        var settleCalls = new AtomicInteger();
        var result =
                ZLinkRelocationHandOff.run(
                        client(settleCalls),
                        request(),
                        () -> {
                            throw new IllegalStateException("relay admission rejected");
                        },
                        Duration.ofSeconds(1),
                        Instant.now());

        assertEquals(1, settleCalls.get());
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                result.toCompletableFuture().getNow(null));
    }

    @Test
    void targetCommitOwnsTheCutoverSubmitTerminal() {
        var cutover = new CompletableFuture<Void>();
        var result =
                ZLinkRelocationHandOff.run(
                        client(
                                new AtomicInteger(),
                                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                                cutover),
                        request(),
                        () -> CompletableFuture.completedFuture(null),
                        Duration.ofSeconds(1),
                        Instant.now());

        assertFalse(result.toCompletableFuture().isDone());
        cutover.complete(null);
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                result.toCompletableFuture().getNow(null));
    }

    @Test
    void sourcePreserveCancelsPendingCutoverSubmit() {
        var cutover = new CompletableFuture<Void>();
        var settlement = new CompletableFuture<ZLinkRelocationTransitionClient.Settlement>();
        var result =
                ZLinkRelocationHandOff.run(
                        client(new AtomicInteger(), settlement, cutover),
                        request(),
                        () -> CompletableFuture.completedFuture(null),
                        Duration.ofSeconds(1),
                        Instant.now());

        settlement.complete(ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED);
        assertEquals(
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                result.toCompletableFuture().getNow(null));
        assertTrue(cutover.isCancelled());
    }

    @Test
    void recordsOneRelocationAndSourceLocalInterruptionAtCutoverTerminal() throws Exception {
        try (var metrics =
                systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe
                        .install()) {
            var cutover = new CompletableFuture<Void>();
            var result =
                    ZLinkRelocationHandOff.run(
                            client(
                                    new AtomicInteger(),
                                    ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                                    cutover),
                            request(),
                            ignored -> CompletableFuture.completedFuture(null),
                            Duration.ofSeconds(1),
                            Instant.now(),
                            System.nanoTime());
            assertEquals(1, metrics.count("zlink.relocation.started"));
            assertEquals(0, metrics.count("zlink.relocation.completed"));
            assertEquals(0, metrics.count("zlink.relocation.interruption"));
            cutover.complete(null);
            assertEquals(
                    ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                    result.toCompletableFuture().join());
            assertEquals(
                    "completed", metrics.only("zlink.relocation.completed").tags().get("outcome"));
            assertEquals(1, metrics.count("zlink.relocation.duration"));
            assertEquals(1, metrics.count("zlink.relocation.bytes"));
            assertTrue(metrics.only("zlink.relocation.interruption").value() >= 0);
            metrics.assertContractLabels();
        }
    }

    @Test
    void cleanupFailureAfterConfirmedCommitCannotReplaceCompleted() throws Exception {
        try (var metrics =
                systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetricProbe
                        .install()) {
            var result =
                    ZLinkRelocationHandOff.run(
                            client(
                                    new AtomicInteger(),
                                    ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                                    CompletableFuture.failedFuture(
                                            new IllegalStateException("cutover cleanup failed"))),
                            request(),
                            () -> CompletableFuture.completedFuture(null),
                            Duration.ofSeconds(1),
                            Instant.now());
            assertEquals(
                    ZLinkRelocationTransitionClient.Settlement.TARGET_COMMITTED,
                    result.toCompletableFuture().join());
            assertEquals(
                    "completed", metrics.only("zlink.relocation.completed").tags().get("outcome"));
            assertEquals(1, metrics.count("zlink.relocation.duration"));
        }
    }

    private static ZLinkSpotRetireControl.StageRequest request() {
        RoutingId source = RoutingId.from("source");
        RoutingId target = RoutingId.from("target");
        var fence = new ZLinkSpotRetireControl.Fence(UUID.randomUUID(), 1);
        return new ZLinkSpotRetireControl.StageRequest(
                fence,
                source,
                1,
                "source-owner",
                1,
                target,
                1,
                "target-owner",
                1,
                "mesh",
                "actor",
                "player",
                false,
                true,
                new byte[] {1},
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                "actor", 1, "actor", "player", true, 1, 1)));
    }

    @Test
    void settlementThrowAfterRelayReadinessBelongsToSourceCleanup() {
        var failure = new IllegalStateException("source settlement failed");
        var ready =
                ZLinkRelocationHandOff.runToReady(
                        client(
                                new AtomicInteger(),
                                () -> {
                                    throw failure;
                                },
                                CompletableFuture.completedFuture(null)),
                        request(),
                        ignored -> CompletableFuture.completedFuture(null),
                        Duration.ofSeconds(1),
                        Instant.now());
        var cleanup = ready.toCompletableFuture().join();
        var error =
                assertThrows(
                        java.util.concurrent.CompletionException.class,
                        () -> cleanup.toCompletableFuture().join());
        assertSame(failure, error.getCause());
    }

    private static ZLinkRelocationTransitionClient client(AtomicInteger settleCalls) {
        return client(
                settleCalls,
                ZLinkRelocationTransitionClient.Settlement.SOURCE_PRESERVED,
                CompletableFuture.completedFuture(null));
    }

    private static ZLinkRelocationTransitionClient client(
            AtomicInteger settleCalls,
            ZLinkRelocationTransitionClient.Settlement settlement,
            CompletionStage<Void> cutover) {
        return client(settleCalls, CompletableFuture.completedFuture(settlement), cutover);
    }

    private static ZLinkRelocationTransitionClient client(
            AtomicInteger settleCalls,
            CompletionStage<ZLinkRelocationTransitionClient.Settlement> settlement,
            CompletionStage<Void> cutover) {
        return client(settleCalls, () -> settlement, cutover);
    }

    private static ZLinkRelocationTransitionClient client(
            AtomicInteger settleCalls,
            java.util.function.Supplier<CompletionStage<ZLinkRelocationTransitionClient.Settlement>>
                    settlement,
            CompletionStage<Void> cutover) {
        return new ZLinkRelocationTransitionClient() {
            @Override
            public CompletionStage<Void> stage(
                    RoutingId rid,
                    ZLinkSpotRetireControl.StageRequest value,
                    Duration timeout,
                    java.time.Instant deadline) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> relay(
                    RoutingId rid,
                    ZLinkSpotRetireControl.Fence value,
                    byte[] record,
                    Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> publish(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Duration timeout) {
                return cutover;
            }

            @Override
            public CompletionStage<Settlement> settle(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Instant preserveAt) {
                settleCalls.incrementAndGet();
                return settlement.get();
            }

            @Override
            public CompletionStage<Void> abort(
                    RoutingId rid, ZLinkSpotRetireControl.Fence value, Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }
        };
    }
}
