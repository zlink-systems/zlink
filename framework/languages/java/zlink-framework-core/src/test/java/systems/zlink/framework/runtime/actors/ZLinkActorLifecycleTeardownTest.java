package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorLifecycleTeardownTest {
    private static final String ACTOR = "player-1";
    private static final long BOUND_SECONDS = 3;

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void hostCloseThenOffTurnSettlementCompletesInsideJoinBarrier(ZLinkUserSpotExecutionMode mode)
            throws Exception {
        verifySettlement(true, mode);
    }

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void offTurnSettlementAloneCompletesInsideJoinBarrier(ZLinkUserSpotExecutionMode mode)
            throws Exception {
        verifySettlement(false, mode);
    }

    private void verifySettlement(boolean hostClose, ZLinkUserSpotExecutionMode mode)
            throws Exception {
        var dispatches = dispatches(mode);
        dispatches.prepareAsync(ACTOR).toCompletableFuture().join();
        var entered = new CompletableFuture<Void>();
        var settled = new CompletableFuture<Void>();
        var cleanupEntered = new CompletableFuture<Void>();
        var cleanupRelease = new CompletableFuture<Void>();
        var handle = new AtomicReference<ZLinkSerialExecutionQueue.ActiveTurnSealHandle>();
        List<String> order = new CopyOnWriteArrayList<>();
        CompletionStage<Void> barrier =
                dispatches.enqueueBarrier(
                        ACTOR,
                        () -> {
                            handle.set(
                                    ZLinkSerialExecutionQueue.captureCurrentActiveTurnSealHandle()
                                            .orElseThrow());
                            entered.complete(null);
                            return settled;
                        });
        CompletionStage<Void> close = CompletableFuture.completedFuture(null);
        try {
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            if (hostClose) {
                close =
                        CompletableFuture.supplyAsync(
                                        () ->
                                                dispatches.beginTeardown(
                                                        ACTOR,
                                                        () -> {
                                                            order.add("host-close");
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }))
                                .get(BOUND_SECONDS, TimeUnit.SECONDS);
                assertFalse(close.toCompletableFuture().isDone());
            }
            CompletionStage<Void> cleanup =
                    CompletableFuture.supplyAsync(
                                    () ->
                                            dispatches.beginTeardown(
                                                    ACTOR,
                                                    handle.get(),
                                                    () -> {
                                                        order.add("settlement");
                                                        cleanupEntered.complete(null);
                                                        return cleanupRelease;
                                                    }))
                            .get(BOUND_SECONDS, TimeUnit.SECONDS);
            cleanupEntered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertFalse(cleanup.toCompletableFuture().isDone());
            assertFalse(barrier.toCompletableFuture().isDone());
            if (hostClose) assertFalse(close.toCompletableFuture().isDone());
            cleanupRelease.complete(null);
            cleanup.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertEquals(List.of("settlement"), order);
            assertFalse(barrier.toCompletableFuture().isDone());
        } finally {
            cleanupRelease.complete(null);
            settled.complete(null);
            barrier.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            close.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            dispatches.awaitQuiescence().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
        assertEquals(
                hostClose ? List.of("settlement", "host-close") : List.of("settlement"), order);
    }

    private ZLinkActorDispatchSerials dispatches(ZLinkUserSpotExecutionMode mode) {
        var target =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        mode,
                        false);
        Object incarnation = new Object();
        return new ZLinkActorDispatchSerials(
                new Object(),
                Runnable::run,
                actorId ->
                        CompletableFuture.completedFuture(
                                new ZLinkActorDispatchTarget.ActivationSnapshot(
                                        target, incarnation)));
    }

    @Test
    void foreignAndExpiredHandlesFailBeforeClosingActorAdmission() throws Exception {
        var dispatches = new ZLinkActorDispatchSerials();
        var first = dispatches.prepareAsync(ACTOR).toCompletableFuture().join();
        var second = dispatches.prepareAsync("player-2").toCompletableFuture().join();
        var entered = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        var handle = new AtomicReference<ZLinkSerialExecutionQueue.ActiveTurnSealHandle>();
        var barrier =
                dispatches.enqueueBarrier(
                        ACTOR,
                        () -> {
                            handle.set(
                                    ZLinkSerialExecutionQueue.captureCurrentActiveTurnSealHandle()
                                            .orElseThrow());
                            entered.complete(null);
                            return release;
                        });
        try {
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            var failure =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    dispatches
                                            .beginTeardown(
                                                    "player-2",
                                                    handle.get(),
                                                    () -> {
                                                        fail("foreign handle must not run cleanup");
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    })
                                            .toCompletableFuture()
                                            .join());
            assertEquals(
                    ZLinkFrameworkErrorKind.INVALID_OPERATION,
                    assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
            dispatches
                    .enqueue(second, () -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture()
                    .get(BOUND_SECONDS, TimeUnit.SECONDS);
        } finally {
            release.complete(null);
            barrier.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
        var failure =
                assertThrows(
                        CompletionException.class,
                        () ->
                                dispatches
                                        .beginTeardown(
                                                ACTOR,
                                                handle.get(),
                                                () -> {
                                                    fail("expired handle must not run cleanup");
                                                    return CompletableFuture.completedFuture(null);
                                                })
                                        .toCompletableFuture()
                                        .join());
        assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                assertInstanceOf(ZLinkFrameworkException.class, failure.getCause()).kind());
        dispatches
                .enqueue(first, () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .get(BOUND_SECONDS, TimeUnit.SECONDS);
    }

    @Test
    void settlementOnJoinBarrierTurnCompletes() throws Exception {
        var dispatches = new ZLinkActorDispatchSerials();
        dispatches.prepareAsync(ACTOR).toCompletableFuture().join();
        dispatches
                .enqueueBarrier(
                        ACTOR,
                        () ->
                                dispatches.beginTeardown(
                                        ACTOR, () -> CompletableFuture.completedFuture(null)))
                .toCompletableFuture()
                .get(BOUND_SECONDS, TimeUnit.SECONDS);
        dispatches.awaitQuiescence().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
    }

    @Test
    void hostCloseWithoutSettlementRunsAfterJoinBarrier() throws Exception {
        var dispatches = new ZLinkActorDispatchSerials();
        dispatches.prepareAsync(ACTOR).toCompletableFuture().join();
        var entered = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        List<String> order = new CopyOnWriteArrayList<>();
        CompletionStage<Void> barrier =
                dispatches.enqueueBarrier(
                        ACTOR,
                        () -> {
                            entered.complete(null);
                            return release.thenRun(() -> order.add("barrier"));
                        });
        CompletionStage<Void> close = CompletableFuture.completedFuture(null);
        try {
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            close =
                    dispatches.beginTeardown(
                            ACTOR,
                            () -> {
                                order.add("host-close");
                                return CompletableFuture.completedFuture(null);
                            });
            assertFalse(close.toCompletableFuture().isDone());
            assertTrue(order.isEmpty());
        } finally {
            release.complete(null);
            barrier.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            close.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
        assertEquals(List.of("barrier", "host-close"), order);
    }
}
