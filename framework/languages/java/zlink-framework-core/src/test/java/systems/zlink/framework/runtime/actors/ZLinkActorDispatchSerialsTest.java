package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorDispatchSerialsTest {
    private enum ForwardResult {
        ACCEPTED,
        REJECTED,
        THROWS,
        ABSENT
    }

    @ParameterizedTest
    @CsvSource({
        "true,ACCEPTED", "true,REJECTED", "true,THROWS", "true,ABSENT",
        "false,ACCEPTED", "false,REJECTED", "false,THROWS", "false,ABSENT"
    })
    void candidateCutCombinesFollowResultBeforeCompletingAdmission(
            boolean lazy, ForwardResult result) {
        var target = actorTarget();
        var dispatches = dispatches(new AtomicReference<>(target));
        var turn = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        var queue = turn.activation().relocationLane();
        var seal = queue.trySealRelocation().toCompletableFuture().join().orElseThrow();
        queue.commitRelocation(seal).toCompletableFuture().join().orElseThrow();
        var calls = new AtomicInteger();
        var redirectFailure = new IllegalStateException("follow failed");
        ZLinkBackendActorReceived.RelocationRedirect redirect =
                result == ForwardResult.ABSENT
                        ? null
                        : (header, payload) -> {
                            calls.incrementAndGet();
                            assertEquals(11, payload.toByteArray()[0]);
                            if (result == ForwardResult.THROWS) throw redirectFailure;
                            return result == ForwardResult.ACCEPTED;
                        };
        try (var header = followHeader(redirect);
                var payload = Message.from(new byte[] {11})) {
            var admission = new CompletableFuture<Void>();
            var terminal =
                    candidate(
                            dispatches,
                            turn,
                            lazy,
                            admission,
                            header,
                            payload,
                            () -> {
                                throw new AssertionError("a cut candidate cannot execute");
                            });
            if (result == ForwardResult.ACCEPTED) {
                terminal.toCompletableFuture().join();
                admission.join();
            } else {
                var failure =
                        assertThrows(
                                        CompletionException.class,
                                        () -> terminal.toCompletableFuture().join())
                                .getCause();
                assertTrue(failure instanceof ZLinkSerialExecutionQueue.RelocatedOwnerException);
                assertSame(
                        failure,
                        assertThrows(CompletionException.class, admission::join).getCause());
                assertEquals(
                        result == ForwardResult.THROWS ? List.of(redirectFailure) : List.of(),
                        java.util.Arrays.asList(failure.getSuppressed()));
            }
            assertEquals(result == ForwardResult.ABSENT ? 0 : 1, calls.get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    void closingCandidateNeverCallsFollow(boolean lazy) {
        var dispatches = dispatches(new AtomicReference<>(actorTarget()));
        var turn = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        turn.activation().relocationLane().sealClosingAdmission();
        try (var header =
                        followHeader(
                                (h, p) -> {
                                    throw new AssertionError("Closing is not relocation");
                                });
                var payload = Message.from(new byte[] {11})) {
            var admission = new CompletableFuture<Void>();
            var terminal =
                    candidate(
                            dispatches,
                            turn,
                            lazy,
                            admission,
                            header,
                            payload,
                            () -> {
                                throw new AssertionError("Closing candidate cannot execute");
                            });
            var failure =
                    assertThrows(
                                    CompletionException.class,
                                    () -> terminal.toCompletableFuture().join())
                            .getCause();
            assertFalse(failure instanceof ZLinkSerialExecutionQueue.RelocatedOwnerException);
            assertSame(
                    failure, assertThrows(CompletionException.class, admission::join).getCause());
        }
    }

    @ParameterizedTest
    @CsvSource({"true,true", "true,false", "false,true", "false,false"})
    void acceptedCandidateDoesNotTurnHandlerFailureIntoAdmissionFailure(
            boolean lazy, boolean follow) {
        var dispatches = dispatches(new AtomicReference<>(actorTarget()));
        var turn = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        var handlerFailure = new IllegalArgumentException("handler failed inline");
        try (var header =
                        followHeader(
                                follow
                                        ? (h, p) -> {
                                            throw new AssertionError(
                                                    "handler failure cannot follow");
                                        }
                                        : null);
                var payload = Message.from(new byte[] {11})) {
            var admission = new CompletableFuture<Void>();
            var terminal =
                    candidate(
                            dispatches,
                            turn,
                            lazy,
                            admission,
                            header,
                            payload,
                            () -> CompletableFuture.failedFuture(handlerFailure));
            admission.join();
            assertSame(
                    handlerFailure,
                    assertThrows(
                                    CompletionException.class,
                                    () -> terminal.toCompletableFuture().join())
                            .getCause());
            admission.join();
        }
    }

    @Test
    void ordinarySpotLazyRejectionStillCompletesAdmission() {
        var queue = new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot());
        queue.sealClosingAdmission();
        var admission = new CompletableFuture<Void>();
        var terminal =
                queue.enqueueRelocatableLazyRecord(
                        () -> new byte[] {1},
                        1,
                        () -> {
                            throw new AssertionError("rejected Spot cannot execute");
                        },
                        () -> {},
                        admission);
        var failure =
                assertThrows(CompletionException.class, () -> terminal.toCompletableFuture().join())
                        .getCause();
        assertSame(failure, assertThrows(CompletionException.class, admission::join).getCause());
    }

    @Test
    void lifecycleFailureStillCompletesAdmission() {
        var target = actorTarget();
        var failure = new IllegalStateException("lifecycle failed");
        var admission = new CompletableFuture<Void>();
        var terminal =
                target.executeActorLifecycleNext(
                        "actor-1", () -> CompletableFuture.failedFuture(failure), admission);
        assertSame(
                failure,
                assertThrows(CompletionException.class, () -> terminal.toCompletableFuture().join())
                        .getCause());
        admission.join();
        var queue = target.claimActorQueue("actor-1").toCompletableFuture().join().relocationLane();
        var seal = queue.trySealRelocation().toCompletableFuture().join().orElseThrow();
        queue.commitRelocation(seal).toCompletableFuture().join().orElseThrow();
        var rejectedAdmission = new CompletableFuture<Void>();
        var rejected =
                target.executeActorLifecycleNext(
                        "actor-1",
                        () -> {
                            throw new AssertionError("relocated lifecycle cannot run");
                        },
                        rejectedAdmission);
        var rejection =
                assertThrows(CompletionException.class, () -> rejected.toCompletableFuture().join())
                        .getCause();
        assertTrue(rejection instanceof ZLinkSerialExecutionQueue.RelocatedOwnerException);
        assertSame(
                rejection,
                assertThrows(CompletionException.class, rejectedAdmission::join).getCause());
    }

    private static CompletionStage<Void> candidate(
            ZLinkActorDispatchSerials dispatches,
            ZLinkActorDispatchTarget.QueuedTurn turn,
            boolean lazy,
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload,
            java.util.function.Supplier<CompletionStage<Void>> operation) {
        return lazy
                ? dispatches.enqueueLazyRecord(
                        turn,
                        () -> new byte[] {1},
                        1,
                        operation,
                        () -> {},
                        admission,
                        header,
                        payload)
                : dispatches.enqueue(turn, 1, operation, () -> {}, admission, header, payload);
    }

    private static ZLinkBackendActorReceived followHeader(
            ZLinkBackendActorReceived.RelocationRedirect redirect) {
        var node = RoutingId.from("source");
        return ZLinkBackendActorReceived.lazyJournal(
                new ZLinkBackendActorRef(node, "actor-1", 1),
                node,
                null,
                java.util.Optional.empty(),
                0,
                0,
                Message.from(new byte[] {1}),
                false,
                () -> new byte[] {1},
                "application/json",
                () -> {},
                redirect);
    }

    @Test
    void failedLogicalRetirementStillObservesFailedDisposalOnce() {
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(actorTarget()));
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        AtomicInteger reports = new AtomicInteger();
        var logicalFailure = new IllegalStateException("logical retirement failed");
        var disposalFailure = new IllegalStateException("scope disposal failed");
        var registration =
                dispatches
                        .registerTeardown(
                                "actor-1",
                                () -> CompletableFuture.failedFuture(logicalFailure),
                                () -> CompletableFuture.failedFuture(disposalFailure))
                        .thenCompose(
                                terminal ->
                                        terminal.whenComplete(
                                                (ignored, failure) -> reports.incrementAndGet()));
        var failure =
                assertThrows(
                        CompletionException.class, () -> registration.toCompletableFuture().join());
        assertSame(logicalFailure, failure.getCause());
        assertEquals(
                List.of(disposalFailure), java.util.Arrays.asList(logicalFailure.getSuppressed()));
        assertEquals(
                1,
                reports.get(),
                "failed disposal must be observed even when logical retirement fails");
    }

    @Test
    void lateHostCloseAfterRelocationRetirementDoesNotResolveAnAbsentActor() {
        var owner = actorTarget();
        var incarnation = new Object();
        var snapshot =
                new AtomicReference<>(
                        new ZLinkActorDispatchTarget.ActivationSnapshot(owner, incarnation));
        var dispatches =
                new ZLinkActorDispatchSerials(
                        new Object(),
                        Runnable::run,
                        actorId -> CompletableFuture.completedFuture(snapshot.get()));
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        var disposed = new AtomicInteger();
        dispatches
                .registerTeardown(
                        "actor-1",
                        () -> {
                            snapshot.set(null);
                            return CompletableFuture.completedFuture(null);
                        },
                        () -> {
                            disposed.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        })
                .thenCompose(terminal -> terminal)
                .toCompletableFuture()
                .join();

        dispatches
                .beginTeardown(
                        "actor-1",
                        incarnation,
                        () -> {
                            disposed.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
        assertEquals(1, disposed.get());
        var rejected = dispatches.prepareAsync("actor-1").toCompletableFuture();
        assertThrows(CompletionException.class, rejected::join);
    }

    @Test
    void teardownJoinsTheReservedIncarnationWhileTargetResolutionWaits() {
        var incarnation = new Object();
        var resolution = new CompletableFuture<ZLinkActorDispatchTarget.ActivationSnapshot>();
        var lookups = new AtomicInteger();
        var dispatches =
                new ZLinkActorDispatchSerials(
                        new Object(),
                        Runnable::run,
                        actorId -> {
                            lookups.incrementAndGet();
                            return resolution;
                        });
        var cleanup = new AtomicInteger();
        java.util.function.Supplier<CompletionStage<Void>> dispose =
                () -> {
                    cleanup.incrementAndGet();
                    return CompletableFuture.completedFuture(null);
                };
        var first = dispatches.beginTeardown("actor-1", incarnation, dispose).toCompletableFuture();
        var second =
                dispatches.beginTeardown("actor-1", incarnation, dispose).toCompletableFuture();
        assertFalse(first.isDone());
        assertFalse(second.isDone());
        var replacement =
                dispatches.beginTeardown("actor-1", new Object(), dispose).toCompletableFuture();
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ((systems.zlink.framework.errors.ZLinkFrameworkException)
                                assertThrows(CompletionException.class, replacement::join)
                                        .getCause())
                        .kind());
        resolution.complete(
                new ZLinkActorDispatchTarget.ActivationSnapshot(actorTarget(), incarnation));
        first.join();
        second.join();
        assertEquals(1, lookups.get());
        assertEquals(1, cleanup.get());
    }

    @Test
    void staleTeardownCannotClaimTheReplacementIncarnation() {
        var incarnation = new Object();
        var target = actorTarget();
        var replacement = new Object();
        var dispatches =
                new ZLinkActorDispatchSerials(
                        new Object(),
                        Runnable::run,
                        actorId ->
                                CompletableFuture.completedFuture(
                                        new ZLinkActorDispatchTarget.ActivationSnapshot(
                                                target, replacement)));
        var closed = new AtomicInteger();
        var terminal =
                dispatches
                        .beginTeardown(
                                "actor-1",
                                incarnation,
                                () -> {
                                    closed.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                })
                        .toCompletableFuture();
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.INVALID_OPERATION,
                ((systems.zlink.framework.errors.ZLinkFrameworkException)
                                assertThrows(CompletionException.class, terminal::join).getCause())
                        .kind());
        assertEquals(0, closed.get());
        var turn = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        assertSame(replacement, turn.incarnation());
        dispatches
                .enqueue(turn, () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
    }

    @Test
    void idleSourceRegistersTeardownBeforeLogicalRetirementAndDisposesAfterIt() {
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(actorTarget()));
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> logicalRetirement = new CompletableFuture<>();
        AtomicInteger disposed = new AtomicInteger();
        var registration =
                dispatches
                        .registerTeardown(
                                "actor-1",
                                () -> logicalRetirement,
                                () -> {
                                    disposed.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                })
                        .toCompletableFuture();
        var terminal = dispatches.awaitTeardown("actor-1").toCompletableFuture();
        try {
            assertFalse(registration.isDone());
            assertFalse(terminal.isDone());
            assertEquals(
                    0,
                    disposed.get(),
                    "disposal must follow bounded logical retirement even on an idle activation");
        } finally {
            logicalRetirement.complete(null);
            registration.join().toCompletableFuture().join();
            terminal.join();
        }
        assertEquals(1, disposed.get());
    }

    @Test
    void relocationLaneUsesCapturedActivationAfterQueueRemoval() {
        ZLinkActorDispatchTarget owner = actorTarget();
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(owner));
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        owner.removeActorQueueAsync("actor-1", claim.activation()).toCompletableFuture().join();
        var recreated = owner.claimActorQueue("actor-1").toCompletableFuture().join();
        assertNotSame(claim.activation().relocationLane(), recreated.relocationLane());
        assertSame(
                claim.activation().relocationLane(),
                dispatches.relocationLaneAsync("actor-1").toCompletableFuture().join());
    }

    @Test
    void teardownRetargetDoesNotCreateUnusedTargetQueue() throws Exception {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> cleanup = new CompletableFuture<>();
        CompletableFuture<Void> cleanupEntered = new CompletableFuture<>();
        var teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupEntered.complete(null);
                            return cleanup;
                        });
        cleanupEntered.join();
        owner.set(target);
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        assertSame(source, claim.target());
        Field queuesField = ZLinkSpotSerialExecutor.class.getDeclaredField("actorQueues");
        queuesField.setAccessible(true);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane targetState = (ZLinkStateLane) laneField.get(target);
        var targetQueues = (java.util.Map<?, ?>) queuesField.get(target);
        try {
            assertTrue(
                    targetState.runNowOrQueue(targetQueues::isEmpty).toCompletableFuture().join());
        } finally {
            cleanup.complete(null);
            teardown.toCompletableFuture().join();
        }
    }

    @Test
    void retiredClaimCannotEnterRecreatedActivation() {
        ZLinkActorDispatchTarget owner = actorTarget();
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(owner));
        var retired = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        dispatches
                .beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        var recreated = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        AtomicInteger calls = new AtomicInteger();
        var stale =
                dispatches.enqueue(
                        retired,
                        () -> {
                            calls.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertThrows(CompletionException.class, () -> stale.toCompletableFuture().join());
        assertEquals(0, calls.get());
        dispatches
                .enqueue(
                        recreated,
                        () -> {
                            calls.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
        assertEquals(1, calls.get());
    }

    @Test
    void prepareAsyncDoesNotBlockWhenMetadataOwnerIsBusy() throws Exception {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        Field field = ZLinkActorDispatchSerials.class.getDeclaredField("stateLane");
        field.setAccessible(true);
        ZLinkStateLane lane = (ZLinkStateLane) field.get(dispatches);
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        lane.runAsync(
                () -> {
                    entered.complete(null);
                    release.join();
                    return null;
                });
        entered.join();
        try {
            var prepared = dispatches.prepareAsync("actor-1");
            assertFalse(prepared.toCompletableFuture().isDone());
            release.complete(null);
            assertEquals("actor-1", prepared.toCompletableFuture().join().actorId());
        } finally {
            release.complete(null);
        }
    }

    @Test
    void preparedClaimKeepsItsQueueOwnerAfterRetarget() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        owner.set(target);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> held = new CompletableFuture<>();
        var submitted = dispatches.enqueue(claim, () -> held);
        assertSame(source, claim.target());
        assertFalse(source.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        assertTrue(target.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        held.complete(null);
        submitted.toCompletableFuture().join();
    }

    @Test
    void teardownRetargetUsesTheSealedActivationQueue() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> cleanupEntered = new CompletableFuture<>();
        CompletableFuture<Void> cleanup = new CompletableFuture<>();
        var teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupEntered.complete(null);
                            return cleanup;
                        });
        cleanupEntered.join();
        try {
            owner.set(target);
            // spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:81:
            // Actor queue owns payload admission; prepare only captures that queue.
            // spec/server/00-foundation/06-framework-api.ko.md:926: admission seal -> Rejected.
            var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
            assertSame(source, claim.target());
            var submitted =
                    dispatches.enqueue(claim, () -> CompletableFuture.completedFuture(null));
            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> submitted.toCompletableFuture().join());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                    ((systems.zlink.framework.errors.ZLinkFrameworkException) failure.getCause())
                            .kind());
            assertTrue(target.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        } finally {
            cleanup.complete(null);
            teardown.toCompletableFuture().join();
        }
    }

    @Test
    void retargetRetainsTheLifecycleBarrierClaim() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> lifecycle = new CompletableFuture<>();
        var barrier =
                dispatches.enqueueBarrier(
                        "actor-1",
                        () -> {
                            entered.complete(null);
                            return lifecycle;
                        });
        entered.join();
        owner.set(target);
        AtomicInteger handlers = new AtomicInteger();
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        var submitted =
                dispatches.enqueue(
                        claim,
                        () -> {
                            handlers.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertFalse(submitted.toCompletableFuture().isDone());
        assertEquals(0, handlers.get());
        lifecycle.complete(null);
        barrier.toCompletableFuture().join();
        submitted.toCompletableFuture().join();
        assertEquals(1, handlers.get());
    }

    @Test
    void retainedCommitWaitsForBusySpotOwnerWithoutBlockingCaller() throws Exception {
        ZLinkSpotSerialExecutor spot = (ZLinkSpotSerialExecutor) actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(spot);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        var seal =
                spot.trySealActorRelocation("actor-1").toCompletableFuture().join().orElseThrow();
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane lane = (ZLinkStateLane) laneField.get(spot);
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                lane.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var retained = dispatches.retainCommitAsync("actor-1", seal).toCompletableFuture();
            assertFalse(retained.isDone());
            release.complete(null);
            var commit = retained.join().orElseThrow();
            systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit.Cut
                    cut;
            do {
                cut = commit.cut().toCompletableFuture().join();
            } while (!commit.tryEstablishAndFinishCapture(cut).toCompletableFuture().join());
            commit.complete();
        } finally {
            release.complete(null);
        }
    }

    @Test
    void teardownRemovalFailureCompletesTerminalExceptionally() {
        ZLinkActorDispatchTarget spot = actorTarget();
        IllegalStateException removalFailure = new IllegalStateException("remove failed");
        ZLinkActorDispatchTarget failing =
                (ZLinkActorDispatchTarget)
                        Proxy.newProxyInstance(
                                ZLinkActorDispatchTarget.class.getClassLoader(),
                                new Class<?>[] {ZLinkActorDispatchTarget.class},
                                (proxy, method, args) ->
                                        method.getName().equals("removeActorQueueAsync")
                                                ? CompletableFuture.failedFuture(removalFailure)
                                                : method.invoke(spot, args));
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(failing));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null));
        CompletionException terminal =
                assertThrows(
                        CompletionException.class, () -> teardown.toCompletableFuture().join());
        assertSame(removalFailure, terminal.getCause());
    }

    @Test
    void teardownKeepsAdmissionClosedUntilSpotRemovalFinishes() {
        ZLinkActorDispatchTarget spot = actorTarget();
        CompletableFuture<Void> removeEntered = new CompletableFuture<>();
        CompletableFuture<Void> removal = new CompletableFuture<>();
        ZLinkActorDispatchTarget delayed =
                (ZLinkActorDispatchTarget)
                        Proxy.newProxyInstance(
                                ZLinkActorDispatchTarget.class.getClassLoader(),
                                new Class<?>[] {ZLinkActorDispatchTarget.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("removeActorQueueAsync")) {
                                        removeEntered.complete(null);
                                        return removal;
                                    }
                                    return method.invoke(spot, args);
                                });
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(delayed));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null));
        removeEntered.join();
        try {
            assertFalse(teardown.toCompletableFuture().isDone());
            // spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:81:
            // The sealed Actor queue rejects admission when enqueue is attempted.
            // spec/server/00-foundation/06-framework-api.ko.md:926: admission seal -> Rejected.
            var rejected =
                    dispatches.enqueue(
                            dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                            () -> CompletableFuture.completedFuture(null));
            CompletionException rejection =
                    assertThrows(
                            CompletionException.class, () -> rejected.toCompletableFuture().join());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                    ((systems.zlink.framework.errors.ZLinkFrameworkException) rejection.getCause())
                            .kind());
        } finally {
            removal.complete(null);
        }
        teardown.toCompletableFuture().join();
    }

    @Test
    void queuedBarrierWaitsForActiveActorDispatch() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> active = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        var first =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            order.add("dispatch-started");
                            return active.thenRun(() -> order.add("dispatch-completed"));
                        });
        var barrier =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            order.add("handoff-started");
                            return CompletableFuture.completedFuture(null);
                        });

        assertFalse(barrier.toCompletableFuture().isDone());
        active.complete(null);
        CompletableFuture.allOf(first.toCompletableFuture(), barrier.toCompletableFuture()).join();

        assertEquals(List.of("dispatch-started", "dispatch-completed", "handoff-started"), order);
    }

    @Test
    void allActorBarrierWaitsForEveryIndependentLane() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> actorA = new CompletableFuture<>();
        CompletableFuture<Void> actorB = new CompletableFuture<>();

        dispatches.enqueue(
                dispatches.prepareAsync("actor-a").toCompletableFuture().join(), () -> actorA);
        dispatches.enqueue(
                dispatches.prepareAsync("actor-b").toCompletableFuture().join(), () -> actorB);

        CompletableFuture<Void> barrier = dispatches.awaitQuiescence().toCompletableFuture();
        assertFalse(barrier.isDone());
        actorA.complete(null);
        assertFalse(barrier.isDone());
        actorB.complete(null);
        barrier.join();
    }

    @Test
    void teardownWaitsForAcceptedTurnsBeforeTheFirstDrain() throws Exception {
        LinkedBlockingQueue<Runnable> ready = new LinkedBlockingQueue<>();
        ZLinkActorDispatchSerials dispatches =
                new ZLinkActorDispatchSerials(new Object(), ready::add, null);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletableFuture<Void> started = new CompletableFuture<>();
        AtomicInteger completedTurns = new AtomicInteger();
        AtomicInteger cleanupCount = new AtomicInteger();
        var active =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            started.complete(null);
                            return release.thenRun(completedTurns::incrementAndGet);
                        });
        var accepted =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            completedTurns.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        var teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            assertEquals(2, completedTurns.get());
                            cleanupCount.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        while (!started.isDone() && !teardown.toCompletableFuture().isDone()) {
            java.util.Objects.requireNonNull(ready.poll(3, TimeUnit.SECONDS)).run();
        }
        try {
            assertFalse(teardown.toCompletableFuture().isDone());
            assertEquals(0, cleanupCount.get());
        } finally {
            release.complete(null);
        }
        var terminal =
                CompletableFuture.allOf(
                        active.toCompletableFuture(),
                        accepted.toCompletableFuture(),
                        teardown.toCompletableFuture());
        while (!terminal.isDone()) {
            java.util.Objects.requireNonNull(ready.poll(3, TimeUnit.SECONDS)).run();
        }
        terminal.join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void teardownWaitsForAcceptedTurnsAndClosesAdmission() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> release = new CompletableFuture<>();
        AtomicInteger cleanupCount = new AtomicInteger();

        CompletionStage<Void> active =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> release);
        CompletionStage<Void> accepted =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> CompletableFuture.completedFuture(null));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupCount.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });

        assertFalse(teardown.toCompletableFuture().isDone());
        var rejected =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> CompletableFuture.completedFuture(null));
        CompletionException rejection =
                assertThrows(
                        CompletionException.class, () -> rejected.toCompletableFuture().join());
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                ((systems.zlink.framework.errors.ZLinkFrameworkException) rejection.getCause())
                        .kind());
        assertEquals(0, cleanupCount.get());

        release.complete(null);
        CompletableFuture.allOf(
                        active.toCompletableFuture(),
                        accepted.toCompletableFuture(),
                        teardown.toCompletableFuture())
                .join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void teardownStartedInsideCurrentTurnDoesNotWaitForItself() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        AtomicInteger cleanupCount = new AtomicInteger();

        CompletionStage<Void> active =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () ->
                                dispatches.beginTeardown(
                                        "actor-1",
                                        () -> {
                                            cleanupCount.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        }));

        active.toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void consecutiveAcceptedPacketTurnsReleaseTheActorLane() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> first =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        new byte[] {1},
                        () -> {
                            order.add("first");
                            return CompletableFuture.completedFuture(null);
                        });
        CompletionStage<Void> second =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        new byte[] {2},
                        () -> {
                            order.add("second");
                            return CompletableFuture.completedFuture(null);
                        });

        CompletableFuture.allOf(first.toCompletableFuture(), second.toCompletableFuture()).join();
        assertEquals(List.of("first", "second"), order);
    }

    @Test
    void yieldedContinuationDoesNotBlockTheNextActorTurn() throws Exception {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            firstStarted.complete(null);
                            return ZLinkSerialExecutionQueue.yieldCurrent(remote);
                        });
        CompletionStage<Void> second =
                dispatches.enqueue(
                        dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                        () -> {
                            secondStarted.complete(null);
                            return CompletableFuture.completedFuture(null);
                        });

        firstStarted.get();
        secondStarted.get();
        assertFalse(first.toCompletableFuture().isDone());

        remote.complete(null);
        CompletableFuture.allOf(first.toCompletableFuture(), second.toCompletableFuture()).join();
    }

    @Test
    void committedQueuesAreRetiredAcrossActorReturnToPreviousSpot() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        ZLinkActorDispatchTarget spotB = actorTarget();
        List<String> order = new ArrayList<>();

        relocateAndRetire(dispatches, owner, spotA, order, "A");
        relocateAndRetire(dispatches, owner, spotB, order, "B");
        owner.set(spotA);

        enqueueLazy(dispatches, order, "A-return").toCompletableFuture().join();

        assertEquals(List.of("A", "B", "A-return"), order);
    }

    @Test
    void removeRetiresLastPreparedTargetAfterResolverLosesActor() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        List<String> order = new ArrayList<>();

        owner.set(spotA);
        enqueueLazy(dispatches, order, "before-remove").toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        var seal =
                dispatches
                        .relocationLaneAsync("actor-1")
                        .toCompletableFuture()
                        .join()
                        .trySealRelocation()
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        dispatches
                .relocationLaneAsync("actor-1")
                .toCompletableFuture()
                .join()
                .commitRelocation(seal)
                .toCompletableFuture()
                .join()
                .orElseThrow();

        owner.set(null);
        dispatches.removeAsync("actor-1").toCompletableFuture().join();
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove").toCompletableFuture().join();

        assertEquals(List.of("before-remove", "after-remove"), order);
    }

    @Test
    void removeRetiresQueueCreatedOnlyForRelocation() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        List<String> order = new ArrayList<>();

        owner.set(spotA);
        dispatches.relocationLaneAsync("actor-1").toCompletableFuture().join();
        var seal =
                dispatches
                        .relocationLaneAsync("actor-1")
                        .toCompletableFuture()
                        .join()
                        .trySealRelocation()
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        dispatches
                .relocationLaneAsync("actor-1")
                .toCompletableFuture()
                .join()
                .commitRelocation(seal)
                .toCompletableFuture()
                .join()
                .orElseThrow();

        owner.set(null);
        dispatches.removeAsync("actor-1").toCompletableFuture().join();
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove").toCompletableFuture().join();

        assertEquals(List.of("after-remove"), order);
    }

    private static ZLinkActorDispatchSerials dispatches(
            AtomicReference<ZLinkActorDispatchTarget> owner) {
        return new ZLinkActorDispatchSerials(
                new Object(),
                Runnable::run,
                actorId ->
                        CompletableFuture.completedFuture(
                                new ZLinkActorDispatchTarget.ActivationSnapshot(
                                        owner.get(), actorId)));
    }

    private static ZLinkActorDispatchTarget actorTarget() {
        return new ZLinkSpotSerialExecutor(
                new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot()),
                Runnable::run,
                Runnable::run,
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                false);
    }

    private static void relocateAndRetire(
            ZLinkActorDispatchSerials dispatches,
            AtomicReference<ZLinkActorDispatchTarget> owner,
            ZLinkActorDispatchTarget target,
            List<String> order,
            String step) {
        owner.set(target);
        enqueueLazy(dispatches, order, step).toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        var seal =
                dispatches
                        .relocationLaneAsync("actor-1")
                        .toCompletableFuture()
                        .join()
                        .trySealRelocation()
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        dispatches
                .relocationLaneAsync("actor-1")
                .toCompletableFuture()
                .join()
                .commitRelocation(seal)
                .toCompletableFuture()
                .join()
                .orElseThrow();
        dispatches
                .beginTeardown(
                        "actor-1",
                        () -> {
                            owner.set(null);
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
    }

    private static CompletionStage<Void> enqueueLazy(
            ZLinkActorDispatchSerials dispatches, List<String> order, String step) {
        return dispatches.enqueueLazyRecord(
                dispatches.prepareAsync("actor-1").toCompletableFuture().join(),
                () -> new byte[] {1},
                1,
                () -> {
                    order.add(step);
                    return CompletableFuture.completedFuture(null);
                },
                () -> {});
    }
}
