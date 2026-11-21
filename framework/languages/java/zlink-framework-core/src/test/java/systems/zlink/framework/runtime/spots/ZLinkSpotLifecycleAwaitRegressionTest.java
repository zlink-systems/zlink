package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;

final class ZLinkSpotLifecycleAwaitRegressionTest {
    private static final long BOUND_SECONDS = 3;

    @Test
    void independentActorInfrastructureWaitRetainsItsMailboxClaim() throws Exception {
        var release = new CompletableFuture<Void>();
        var observer = new AtomicReference<Runnable>();
        var retained = new AtomicBoolean();
        var payloadEntered = new AtomicBoolean();
        var executor = Executors.newSingleThreadExecutor();
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(
                                Runnable::run, ZLinkExecutionLanePolicy.spot()),
                        executor,
                        executor,
                        ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        var actor =
                serials.claimActorQueue("actor")
                        .toCompletableFuture()
                        .get(BOUND_SECONDS, TimeUnit.SECONDS);
        try {
            var lifecycle =
                    actor.executeLifecycleNext(
                            () -> {
                                ZLinkSerialExecutionQueue.propagateCurrent(observer::set)
                                        .execute(
                                                () ->
                                                        retained.set(
                                                                actor.relocationLane()
                                                                        .isCurrent()));
                                return ZLinkSerialExecutionQueue.yieldCurrent(release);
                            },
                            null);
            executor.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
            var payload =
                    actor.executeActor(
                            () -> {
                                payloadEntered.set(true);
                                return CompletableFuture.completedFuture(null);
                            },
                            null);
            executor.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
            boolean payloadOverlapped = payloadEntered.get();
            serials.executeInfrastructure(() -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture()
                    .get(BOUND_SECONDS, TimeUnit.SECONDS);
            serials.executeTimer("probe", ignored -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture()
                    .get(BOUND_SECONDS, TimeUnit.SECONDS);
            observer.get().run();
            boolean owned = retained.get();
            release.complete(null);
            lifecycle.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            payload.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertFalse(
                    payloadOverlapped,
                    "the next Actor payload must remain behind its pending lifecycle result");
            assertTrue(payloadEntered.get(), "Actor payload must progress after lifecycle release");
            assertTrue(
                    owned,
                    "infrastructure progress must not release an independent Actor mailbox claim");
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            executor.shutdownNow();
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void infrastructureYieldThenOrdinaryAwaitRetainsTheResumedGate(boolean lifecycle)
            throws Exception {
        var infrastructure = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var observeTurn = new AtomicReference<Runnable>();
        var held = new AtomicBoolean();
        var siblingEntered = new AtomicBoolean();
        var executor = Executors.newSingleThreadExecutor();
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue, executor, executor, ZLinkUserSpotExecutionMode.SPOT_WIDE, false);
        try {
            java.util.function.Supplier<CompletionStage<Void>> operation =
                    () -> {
                        var stage =
                                ZLinkSerialExecutionQueue.yieldCurrent(infrastructure)
                                        .thenCompose(
                                                ignored -> {
                                                    ZLinkSerialExecutionQueue.propagateCurrent(
                                                                    observeTurn::set)
                                                            .execute(
                                                                    () ->
                                                                            held.set(
                                                                                    queue
                                                                                            .isCurrent()));
                                                    return release;
                                                });
                        entered.complete(null);
                        return stage;
                    };
            var turn =
                    lifecycle
                            ? serials.enqueueSpotLifecycleAdmission(operation)
                            : serials.executeTimer("resumed-timer", ignored -> operation.get());
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            executor.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
            infrastructure.complete(null);
            executor.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
            observeTurn.get().run();
            boolean retained = held.get();
            var sibling =
                    serials.executeTimer(
                            "sibling",
                            ignored -> {
                                siblingEntered.set(true);
                                return CompletableFuture.completedFuture(null);
                            });
            executor.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
            boolean siblingOverlapped = siblingEntered.get();
            release.complete(null);
            turn.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            sibling.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertFalse(
                    siblingOverlapped,
                    "a resumed callback retains the gate through ordinary await");
            assertTrue(siblingEntered.get(), "sibling must run after the callback completes");
            assertTrue(
                    retained,
                    "ordinary callback await after infrastructure Yield must retain the resumed gate");
        } finally {
            infrastructure.complete(null);
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            executor.shutdownNow();
        }
    }

    @ParameterizedTest
    @EnumSource(ZLinkUserSpotExecutionMode.class)
    void actorLifecycleInfrastructureWaitCanSealAndResumeItsRelocation(
            ZLinkUserSpotExecutionMode mode) throws Exception {
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var prefixReturned = new CompletableFuture<Void>();
        var handle = new AtomicReference<ZLinkSerialExecutionQueue.ActiveTurnSealHandle>();
        var seal = new AtomicReference<ZLinkSerialExecutionQueue.RelocationSeal>();
        java.util.concurrent.Executor executor =
                command -> {
                    command.run();
                    prefixReturned.complete(null);
                };
        var queue = new ZLinkSerialExecutionQueue(executor, ZLinkExecutionLanePolicy.spot());
        var serials = new ZLinkSpotSerialExecutor(queue, Runnable::run, executor, mode, false);
        var actor =
                serials.claimActorQueue("actor")
                        .toCompletableFuture()
                        .get(BOUND_SECONDS, TimeUnit.SECONDS);
        var lifecycle =
                actor.executeLifecycleNext(
                        () -> {
                            handle.set(
                                    ZLinkSerialExecutionQueue.captureCurrentActiveTurnSealHandle()
                                            .orElseThrow());
                            var waiting =
                                    ZLinkSerialExecutionQueue.yieldCurrent(release)
                                            .thenCompose(
                                                    ignored ->
                                                            actor.relocationLane()
                                                                    .abortRelocation(seal.get())
                                                                    .thenAccept(
                                                                            aborted ->
                                                                                    assertTrue(
                                                                                            aborted)));
                            entered.complete(null);
                            return waiting;
                        },
                        null);
        try {
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            prefixReturned.get(BOUND_SECONDS, TimeUnit.SECONDS);
            var captured =
                    actor.relocationLane()
                            .trySealRelocation(handle.get())
                            .toCompletableFuture()
                            .get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertTrue(
                    captured.isPresent(),
                    "live logical lifecycle turn must own its relocation seal after Yield");
            seal.set(captured.orElseThrow());
            release.complete(null);
            lifecycle.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        } finally {
            if (seal.get() != null)
                actor.relocationLane()
                        .abortRelocation(seal.get())
                        .toCompletableFuture()
                        .get(BOUND_SECONDS, TimeUnit.SECONDS);
            release.complete(null);
            lifecycle
                    .handle((ignored, failure) -> null)
                    .toCompletableFuture()
                    .get(BOUND_SECONDS, TimeUnit.SECONDS);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
    }

    @Test
    void sharedActorLifecycleYieldPreservesItsMailboxSealHandle() throws Exception {
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var queue = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var actor =
                serials.claimActorQueue("actor")
                        .toCompletableFuture()
                        .get(BOUND_SECONDS, TimeUnit.SECONDS);
        try {
            var lifecycle =
                    actor.executeLifecycleNext(
                            () -> {
                                var waiting =
                                        ZLinkSerialExecutionQueue.yieldCurrent(release)
                                                .thenCompose(
                                                        ignored -> {
                                                            var handle =
                                                                    ZLinkSerialExecutionQueue
                                                                            .captureCurrentActiveTurnSealHandle()
                                                                            .orElseThrow();
                                                            return actor.relocationLane()
                                                                    .trySealRelocation(handle)
                                                                    .thenCompose(
                                                                            seal -> {
                                                                                assertTrue(
                                                                                        seal
                                                                                                .isPresent(),
                                                                                        "resumed Actor turn must retain its Actor mailbox identity");
                                                                                return actor.relocationLane()
                                                                                        .abortRelocation(
                                                                                                seal
                                                                                                        .orElseThrow())
                                                                                        .thenAccept(
                                                                                                aborted ->
                                                                                                        assertTrue(
                                                                                                                aborted));
                                                                            });
                                                        });
                                entered.complete(null);
                                return waiting;
                            },
                            null);
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            release.complete(null);
            lifecycle.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void closeStepYieldsOnlyForInfrastructure(boolean userCallback) throws Exception {
        var release = new CompletableFuture<Void>();
        var stepPrefixReturned = new CompletableFuture<Void>();
        var observeTurn = new AtomicReference<Runnable>();
        var gateHeld = new AtomicBoolean();
        var queue =
                new ZLinkSerialExecutionQueue(
                        command -> {
                            command.run();
                            // The commit can yield before the step registers its continuation.
                            if (observeTurn.get() != null) stepPrefixReturned.complete(null);
                        },
                        ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        java.util.function.Supplier<CompletionStage<Void>> operation =
                () -> {
                    ZLinkSerialExecutionQueue.propagateCurrent(observeTurn::set)
                            .execute(() -> gateHeld.set(queue.isCurrent()));
                    return release;
                };
        var coordinator =
                new ZLinkSpotCloseCoordinator(
                        () -> CompletableFuture.completedFuture(true),
                        List.of(
                                userCallback
                                        ? ZLinkSpotCloseCoordinator.Step.onClosing(operation)
                                        : ZLinkSpotCloseCoordinator.Step.operation(operation)),
                        failure -> {
                            throw new AssertionError(failure);
                        });
        try {
            var closed =
                    serials.enqueueClose(
                            () -> coordinator.close().thenApply(ignored -> null),
                            coordinator::committed);
            stepPrefixReturned.get(BOUND_SECONDS, TimeUnit.SECONDS);
            observeTurn.get().run();
            boolean retained = gateHeld.get();
            release.complete(null);
            closed.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            if (userCallback) {
                assertTrue(retained, "OnClosing must retain the Spot gate across ordinary await");
            } else {
                assertFalse(retained, "infrastructure step must yield the Spot gate");
            }
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
    }

    private enum ClosingOutcome {
        SUCCESS,
        SYNCHRONOUS_FAILURE,
        ASYNCHRONOUS_FAILURE
    }

    @ParameterizedTest
    @EnumSource(ClosingOutcome.class)
    void onClosingSharesItsResultWithoutHoldingTheActivationMonitor(ClosingOutcome outcome)
            throws Exception {
        var activation = new ZLinkInstanceSpotActivation(null, null, null, null, null);
        var callbackEntered = new CompletableFuture<Boolean>();
        var releaseCallback = new CompletableFuture<Void>();
        var callbackResult = new CompletableFuture<Void>();
        var callbackCalls = new AtomicInteger();
        var cleanupCalls = new AtomicInteger();
        var diagnostic = new AtomicReference<Throwable>();
        var failure = new IllegalStateException("OnClosing failed");
        var application = Executors.newSingleThreadExecutor();
        var queue = new ZLinkSerialExecutionQueue(application, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        Supplier<CompletionStage<Void>> onClosing =
                () -> {
                    callbackCalls.incrementAndGet();
                    callbackEntered.complete(Thread.holdsLock(activation));
                    assertTrue(queue.isCurrent());
                    releaseCallback.join();
                    if (outcome == ClosingOutcome.SYNCHRONOUS_FAILURE) {
                        throw failure;
                    }
                    return callbackResult;
                };
        Supplier<CompletionStage<Void>> unexpectedCallback =
                () -> {
                    throw new AssertionError("OnClosing ran again");
                };
        var coordinator =
                new ZLinkSpotCloseCoordinator(
                        () -> CompletableFuture.completedFuture(true),
                        List.of(
                                ZLinkSpotCloseCoordinator.Step.onClosing(
                                        () -> activation.closingCallback(onClosing)),
                                ZLinkSpotCloseCoordinator.Step.operation(
                                        () -> {
                                            cleanupCalls.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        })),
                        diagnostic::set);
        activation.closeCoordinator(() -> coordinator);
        try (application;
                var observer = Executors.newSingleThreadExecutor()) {
            try {
                var closed =
                        serials.enqueueClose(
                                () -> coordinator.close().thenApply(ignored -> null),
                                coordinator::committed);
                assertFalse(
                        callbackEntered.get(BOUND_SECONDS, TimeUnit.SECONDS),
                        "OnClosing must run outside the activation monitor");
                var duplicate =
                        observer.submit(
                                () -> {
                                    assertTrue(activation.closeStarted());
                                    assertSame(coordinator, activation.existingCloseCoordinator());
                                    return activation.closingCallback(unexpectedCallback);
                                });
                var shared = duplicate.get(BOUND_SECONDS, TimeUnit.SECONDS);
                var duplicateClose = coordinator.close().toCompletableFuture();
                assertFalse(shared.toCompletableFuture().isDone());
                assertFalse(duplicateClose.isDone());
                assertEquals(0, cleanupCalls.get());

                releaseCallback.complete(null);
                application.submit(() -> {}).get(BOUND_SECONDS, TimeUnit.SECONDS);
                if (outcome == ClosingOutcome.ASYNCHRONOUS_FAILURE) {
                    callbackResult.completeExceptionally(failure);
                } else {
                    callbackResult.complete(null);
                }
                closed.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
                assertTrue(duplicateClose.get(BOUND_SECONDS, TimeUnit.SECONDS));
                assertSame(shared, activation.closingCallback(unexpectedCallback));
                Throwable expected = outcome == ClosingOutcome.SUCCESS ? null : failure;
                assertSame(
                        expected,
                        shared.handle((ignored, error) -> error)
                                .toCompletableFuture()
                                .get(BOUND_SECONDS, TimeUnit.SECONDS));
                assertSame(expected, diagnostic.get());
                assertEquals(1, callbackCalls.get());
                assertEquals(1, cleanupCalls.get());
            } finally {
                releaseCallback.complete(null);
                callbackResult.complete(null);
                serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void explicitInfrastructureYieldKeepsLifecycleFifoAndResumesThroughGate() throws Exception {
        var release = new CompletableFuture<Void>();
        var entered = new CompletableFuture<Void>();
        var resumed = new AtomicBoolean();
        var nextEntered = new AtomicBoolean();
        var queue = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        try {
            var first =
                    serials.enqueueSpotLifecycleAdmission(
                            () -> {
                                var waiting =
                                        ZLinkSerialExecutionQueue.yieldCurrent(release)
                                                .thenRun(() -> resumed.set(queue.isCurrent()));
                                entered.complete(null);
                                return waiting;
                            });
            entered.get(BOUND_SECONDS, TimeUnit.SECONDS);
            var next =
                    serials.enqueueSpotLifecycleAdmission(
                            () -> {
                                nextEntered.set(true);
                                return CompletableFuture.completedFuture(null);
                            });
            serials.executeTimer("probe", ignored -> CompletableFuture.completedFuture(null))
                    .toCompletableFuture()
                    .get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertFalse(first.toCompletableFuture().isDone());
            assertFalse(
                    nextEntered.get(), "Yield must preserve the lifecycle operation's FIFO slot");
            release.complete(null);
            first.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            next.toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
            assertTrue(resumed.get(), "infrastructure completion must reacquire the Spot gate");
            assertTrue(nextEntered.get());
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(BOUND_SECONDS, TimeUnit.SECONDS);
        }
    }

    @Test
    void joinedActorCallbackRetainsSpotGateAcrossOrdinaryAwait() {
        verifySpotCallback(Callback.JOINED);
    }

    @ParameterizedTest
    @EnumSource(
            value = Callback.class,
            names = {"ADMISSION", "LEAVE", "INITIALIZE"})
    void spotCallbackRetainsGateAcrossOrdinaryAwait(Callback callback) {
        verifySpotCallback(callback);
    }

    enum Callback {
        ADMISSION,
        JOINED,
        LEAVE,
        INITIALIZE
    }

    private void verifySpotCallback(Callback callback) {
        var release = new CompletableFuture<Void>();
        var prefixReturned = new CompletableFuture<Void>();
        var observeTurn = new AtomicReference<Runnable>();
        var gateHeld = new AtomicBoolean();
        var timerEntered = new AtomicBoolean();
        var queue =
                new ZLinkSerialExecutionQueue(
                        command -> {
                            command.run();
                            prefixReturned.complete(null);
                        },
                        ZLinkExecutionLanePolicy.spot());
        var spot =
                new ZLinkSpot<ZLinkActor>() {
                    public ZLinkSpotContext context() {
                        return null;
                    }

                    private CompletionStage<Void> hold() {
                        ZLinkSerialExecutionQueue.propagateCurrent(observeTurn::set)
                                .execute(() -> gateHeld.set(queue.isCurrent()));
                        return release;
                    }

                    public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(
                            String actorId, ZLinkMessage request) {
                        return hold().thenApply(ignored -> ZLinkSpotActorJoinResult.accept());
                    }

                    public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
                        return hold();
                    }

                    public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
                        return hold();
                    }

                    public CompletionStage<Void> onInitialize() {
                        return hold();
                    }
                };
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        try {
            var joined =
                    serials.enqueueSpotLifecycleAdmission(
                            () ->
                                    switch (callback) {
                                        case ADMISSION ->
                                                spot.onActorJoin("probe", null)
                                                        .thenApply(ignored -> null);
                                        case JOINED -> spot.onJoinedActor(null);
                                        case LEAVE -> spot.onLeaveActor(null);
                                        case INITIALIZE -> spot.onInitialize();
                                    });
            prefixReturned.join();
            observeTurn.get().run();
            boolean retainedAcrossAwait = gateHeld.get();
            var timer =
                    serials.executeTimer(
                            "probe",
                            ignored -> {
                                timerEntered.set(true);
                                return CompletableFuture.completedFuture(null);
                            });
            boolean timerOverlapped = timerEntered.get();
            release.complete(null);
            joined.toCompletableFuture().join();
            timer.toCompletableFuture().join();
            assertTrue(timerEntered.get(), "timer must progress after callback release");
            assertFalse(timerOverlapped, "timer must wait for the lifecycle callback to complete");
            assertTrue(
                    retainedAcrossAwait,
                    callback + " callback lost its shared Spot gate during ordinary await");
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().join();
        }
    }
}
