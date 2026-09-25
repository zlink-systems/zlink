package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledFuture;
import java.util.function.Supplier;

final class ZLinkInstanceSpotActivation extends SpotActivationBase<DefaultInstanceSpotContext> {
    private final ZLinkInstanceSpot spot;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private boolean resourcesClosed;
    private ScheduledFuture<?> idleCheck;
    private long idleTimeoutNanos;
    private long lastActivityNanos = System.nanoTime();
    private String expectedOwnerId;
    private long expectedOwnerLeaseGeneration = -1;
    private long expectedAuthorityOwnerGeneration = -1;
    private long expectedNodeGeneration = -1;
    // expectedNodeGeneration carries a node lifecycle-generation opaque
    // equality token (.NET ulong, spec 01-glossary "Lifecycle generation"):
    // full 64-bit range, only zero is unassigned, so a value with bit 63 set
    // decodes to a negative Java long that is a LEGITIMATE token. Unlike
    // expectedOwnerLeaseGeneration/expectedAuthorityOwnerGeneration (spec
    // "OwnerLeaseGeneration"/"AuthorityOwnerGeneration": contractually
    // bounded to 1..long.MaxValue, so a negative sentinel can never collide
    // with a real value), -1 cannot safely double as "not yet set" for this
    // field. authorityFenceEstablished is the non-sign-based presence flag.
    private boolean authorityFenceEstablished;
    private String sealedStoreVersion;

    ZLinkInstanceSpotActivation(
            ZLinkSpotRuntime host,
            ZLinkSpotHandlerInvoker handlerInvoker,
            ZLinkInstanceSpot spot,
            ZLinkBackendSpot backendSpot,
            DefaultInstanceSpotContext context) {
        super(host, handlerInvoker, spot, backendSpot, context);
        this.spot = spot;
    }

    private <T> T inStateLane(Supplier<T> work) {
        try {
            return stateLane.runAsync(work).toCompletableFuture().join();
        } catch (CompletionException failure) {
            Throwable cause = failure.getCause();
            if (cause instanceof RuntimeException runtimeFailure) {
                throw runtimeFailure;
            }
            if (cause instanceof Error error) {
                throw error;
            }
            throw failure;
        }
    }

    private <T> CompletionStage<T> onStateLane(Supplier<T> work) {
        return stateLane.runAsync(work);
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        return onStateLane(
                        () ->
                                handleDispatchEventOnLane(info)
                                        ? drainRoutes()
                                        : CompletableFuture.<Void>completedFuture(null))
                .thenCompose(stage -> stage)
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) {
                                onStateLane(
                                        () -> {
                                            lastActivityNanos = System.nanoTime();
                                            return null;
                                        });
                            }
                        });
    }

    private boolean handleDispatchEventOnLane(ZLinkBackendSpotDispatchInfo info) {
        if (info.event() != ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            return false;
        }
        // The route drain only reads frames and submits each payload after its
        // header has been admitted. Wrapping the whole drain in the same
        // queue would make the payload admission wait behind its own active
        // turn.
        return true;
    }

    void setAuthorityFence(
            String ownerId,
            long ownerLeaseGeneration,
            long authorityOwnerGeneration,
            long nodeGeneration) {
        inStateLane(
                () -> {
                    expectedOwnerId = ownerId;
                    expectedOwnerLeaseGeneration = ownerLeaseGeneration;
                    expectedAuthorityOwnerGeneration = authorityOwnerGeneration;
                    expectedNodeGeneration = nodeGeneration;
                    authorityFenceEstablished = true;
                    return null;
                });
    }

    boolean authorityFenceMatches(
            String ownerId, long ownerLeaseGeneration, long authorityOwnerGeneration) {
        return inStateLane(
                () ->
                        (expectedOwnerId == null || expectedOwnerId.equals(ownerId))
                                && (expectedOwnerLeaseGeneration < 0
                                        || expectedOwnerLeaseGeneration == ownerLeaseGeneration)
                                && (expectedAuthorityOwnerGeneration < 0
                                        || expectedAuthorityOwnerGeneration
                                                == authorityOwnerGeneration));
    }

    void markSealedStoreVersion(String storeVersion) {
        inStateLane(
                () -> {
                    sealedStoreVersion = storeVersion;
                    return null;
                });
    }

    String sealedStoreVersion() {
        return inStateLane(() -> sealedStoreVersion);
    }

    long expectedNodeGeneration() {
        return inStateLane(() -> expectedNodeGeneration);
    }

    boolean hasExpectedNodeGeneration() {
        return inStateLane(() -> authorityFenceEstablished);
    }

    long expectedAuthorityOwnerGeneration() {
        return inStateLane(() -> expectedAuthorityOwnerGeneration);
    }

    systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceRouteFence
            authorityRouteFence() {
        return inStateLane(this::authorityRouteFenceOnLane);
    }

    private systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                    .InstanceRouteFence
            authorityRouteFenceOnLane() {
        if (!authorityFenceEstablished) {
            // Guards against ever shipping the unset -1 sentinel as a node
            // lifecycle-generation opaque token: unlike a bounded field, -1
            // is not distinguishable from a legitimate negative-as-long
            // token here, so a fence built before setAuthorityFence() would
            // be indistinguishable from a real (and wrong) value.
            throw new IllegalStateException(
                    "Instance Spot authority fence requested before it was established");
        }
        return new systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                .InstanceRouteFence(
                context.nodeRid(),
                expectedNodeGeneration,
                context.spotId(),
                context.objectGeneration(),
                expectedOwnerId,
                expectedAuthorityOwnerGeneration,
                expectedOwnerLeaseGeneration,
                sealedStoreVersion == null ? "" : sealedStoreVersion);
    }

    void startIdleEviction(Duration timeout) {
        if (timeout == null || timeout.isZero()) {
            return;
        }
        long nanos;
        try {
            nanos = timeout.toNanos();
        } catch (ArithmeticException overflow) {
            nanos = Long.MAX_VALUE;
        }
        long timeoutNanos = Math.max(1L, nanos);
        IdleSchedule schedule =
                inStateLane(
                        () -> {
                            idleTimeoutNanos = timeoutNanos;
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = null;
                            return new IdleSchedule(previous, idleTimeoutNanos);
                        });
        if (schedule.previous() != null) {
            schedule.previous().cancel(false);
        }
        scheduleIdleCheck(schedule.delayNanos());
    }

    private void scheduleIdleCheck(long delayNanos) {
        ScheduledFuture<?> scheduled =
                host.scheduleInstanceSpotIdleCheck(this::idleTimerFired, Math.max(1L, delayNanos));
        ScheduledFuture<?> discarded =
                inStateLane(
                        () -> {
                            if (idleTimeoutNanos <= 0 || closeStarted() || resourcesClosed) {
                                return scheduled;
                            }
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = scheduled;
                            return previous;
                        });
        if (discarded != null) {
            discarded.cancel(false);
        }
    }

    private boolean canScheduleIdleCheckOnLane() {
        if (idleTimeoutNanos <= 0 || closeStarted() || resourcesClosed) {
            return false;
        }
        return true;
    }

    private void idleTimerFired() {
        onStateLane(
                        () -> {
                            idleCheck = null;
                            return isIdleCandidateOnLane();
                        })
                .thenAccept(
                        idleCandidate -> {
                            if (!idleCandidate) {
                                rescheduleIdleCheck();
                                return;
                            }
                            context.awaitQuiescence()
                                    .whenComplete(
                                            (ignored, failure) -> {
                                                boolean stillIdle =
                                                        failure == null
                                                                && inStateLane(
                                                                        this
                                                                                ::isIdleCandidateOnLane);
                                                if (!stillIdle) {
                                                    rescheduleIdleCheck();
                                                    return;
                                                }
                                                closeWithReason(ZLinkSpotCloseReason.IDLE_EVICTED);
                                            });
                        });
    }

    private boolean isIdleCandidateOnLane() {
        long timeout = idleTimeoutNanos;
        return timeout > 0
                && !host.isClosing()
                && !host.isRelocating()
                && !closeStarted()
                && System.nanoTime() - lastActivityNanos >= timeout
                && !hasActiveRouteReceives()
                && !context.hasActiveTimers();
    }

    private void rescheduleIdleCheck() {
        long delayNanos = inStateLane(() -> canScheduleIdleCheckOnLane() ? idleTimeoutNanos : 0L);
        if (delayNanos > 0) {
            scheduleIdleCheck(delayNanos);
        }
    }

    private CompletionStage<Void> drainRoutes() {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            ZLinkBackendReceived received = backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
            if (received == null) {
                return tail;
            }
            batch.record(
                    ZLinkReceiveBatchBudget.bytesOf(
                            received.parts(),
                            received.applicationMetadataSize(),
                            received.acceptedJournalRecordSize()));
            trackRouteReceived(received);
            ParsedPacket packet;
            try {
                packet = ZLinkSpotRuntime.parsePacket(received.parts());
            } catch (systems.zlink.framework.errors.ZLinkFrameworkException invalidEnvelope) {
                //  A JSON-object first frame that is not a valid shared
                //  envelope is a protocol error (C++ decode parity).
                failRouteInvalidFlow(received, invalidEnvelope);
                continue;
            } catch (RuntimeException invalid) {
                closeRouteReceived(received);
                continue;
            }
            CompletionStage<Void> dispatched = dispatchSpotRouteHandler(received, packet);
            tail = tail.thenCombine(dispatched, (ignored, done) -> null);
        }
        return tail;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail, Supplier<CompletionStage<Void>> operation) {
        return tail.thenCompose(ignored -> operation.get());
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation) {
        return tail.thenCompose(ignored -> context.enqueueDispatch(payloadBytes, operation));
    }

    @Override
    CompletionStage<Void> appendActorLifecycle(
            CompletionStage<Void> tail,
            ZLinkBackendActorLifecycleEvent event,
            ZLinkBackendActorRef actorRef,
            ZLinkActor actor) {
        return CompletableFuture.failedFuture(
                new IllegalStateException("Instance Spot does not own Actor lifecycle"));
    }

    void close(ZLinkSpotCloseReason reason, Instant deadline) {
        inStateLane(
                () -> {
                    backendSpot.sealSpotAdmission(
                            () -> host.spotAdmissionFailure(context.spotId()));
                    drainRoutes();
                    return null;
                });
        try {
            notifyClosing(reason, deadline);
        } finally {
            closeResources();
        }
    }

    void notifyClosing(ZLinkSpotCloseReason reason, Instant deadline) {
        host.awaitClosing(
                closingCallback(
                        () ->
                                context.runClosing(
                                        () ->
                                                spot.onClosing(
                                                        new ZLinkSpotClosingContext(
                                                                reason, deadline)))));
    }

    CompletionStage<Boolean> closeExplicit() {
        return closeWithReason(ZLinkSpotCloseReason.EXPLICIT_CLOSE);
    }

    private CompletionStage<Boolean> closeWithReason(ZLinkSpotCloseReason reason) {
        boolean initiatedInsideTurn = context.isCurrentDispatchTurn();
        ZLinkSpotCloseCoordinator existing = existingCloseCoordinator();
        if (existing != null) {
            return existing.close();
        }
        CloseStart start =
                inStateLane(
                        () -> {
                            boolean retryIdle =
                                    reason == ZLinkSpotCloseReason.IDLE_EVICTED
                                            && (!isIdleCandidateOnLane()
                                                    || context.hasActiveTimers()
                                                    || hasActiveRouteReceives());
                            if (retryIdle) {
                                return new CloseStart(true, null);
                            }
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = null;
                            return new CloseStart(false, previous);
                        });
        if (start.cancelledIdleCheck() != null) {
            start.cancelledIdleCheck().cancel(false);
        }
        if (start.retryIdle()) {
            rescheduleIdleCheck();
            return CompletableFuture.completedFuture(false);
        }
        long ownerGeneration = expectedAuthorityOwnerGeneration();
        var coordinatorOwner =
                new java.util.concurrent.atomic.AtomicReference<ZLinkSpotCloseCoordinator>();
        ZLinkSpotCloseCoordinator coordinator =
                closeCoordinator(
                        () ->
                                new ZLinkSpotCloseCoordinator(
                                        () ->
                                                host.sealInstanceSpotAuthority(
                                                        this,
                                                        () ->
                                                                coordinatorOwner
                                                                        .get()
                                                                        .markCommitted()),
                                        List.of(
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            backendSpot.sealSpotAdmission(
                                                                    () ->
                                                                            host
                                                                                    .spotAdmissionFailure(
                                                                                            context
                                                                                                    .spotId()));
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            context.sealTimerAdmission();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            drainRoutes();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.onClosing(
                                                        () ->
                                                                closingCallback(
                                                                        () ->
                                                                                context.runClosing(
                                                                                        initiatedInsideTurn,
                                                                                        () ->
                                                                                                context
                                                                                                        .runLifecycleExecution(
                                                                                                                () ->
                                                                                                                        spot
                                                                                                                                .onClosing(
                                                                                                                                        new ZLinkSpotClosingContext(
                                                                                                                                                reason,
                                                                                                                                                Instant
                                                                                                                                                        .now())))))),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            backendSpot.closeInstanceSpot();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            closeActiveRouteReceives();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            context.closeTimers();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            context.closeHandlerInstances();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            context.closeBackendSpot();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            host.retireInstanceSpotActivation(this);
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () ->
                                                                host.completeInstanceSpotClose(this)
                                                                        .thenApply(
                                                                                released -> {
                                                                                    if (!released) {
                                                                                        throw new IllegalStateException(
                                                                                                "Instance Spot authority changed during Close");
                                                                                    }
                                                                                    host
                                                                                            .releaseClosingCoordinator(
                                                                                                    context
                                                                                                            .spotId(),
                                                                                                    context
                                                                                                            .objectGeneration(),
                                                                                                    ownerGeneration,
                                                                                                    coordinatorOwner
                                                                                                            .get());
                                                                                    return null;
                                                                                }))),
                                        host.infrastructureExecutor(),
                                        failure ->
                                                host.reportSpotClosingFailure(
                                                        context.spotId(), failure)));
        coordinatorOwner.set(coordinator);
        host.retainClosingCoordinator(
                context.spotId(), context.objectGeneration(), ownerGeneration, coordinator);
        return coordinator
                .close()
                .thenCompose(
                        closed ->
                                !closed && !coordinator.committed()
                                        ? host.discardStaleInstanceSpotActivation(this)
                                        : CompletableFuture.completedFuture(closed))
                .whenComplete(
                        (closed, failure) -> {
                            if (coordinator.finished()
                                    || (!coordinator.committed()
                                            && !ZLinkSpotCloseCoordinator.isUncertainCommit(
                                                    failure))) {
                                host.releaseClosingCoordinator(
                                        context.spotId(),
                                        context.objectGeneration(),
                                        ownerGeneration,
                                        coordinator);
                            }
                            if (!coordinator.committed()
                                    && !ZLinkSpotCloseCoordinator.isUncertainCommit(failure)) {
                                clearUncommittedClose(coordinator);
                                if (reason == ZLinkSpotCloseReason.IDLE_EVICTED) {
                                    rescheduleIdleCheck();
                                }
                            }
                        });
    }

    void closeResources() {
        CloseResources start =
                inStateLane(
                        () -> {
                            if (resourcesClosed) {
                                return null;
                            }
                            resourcesClosed = true;
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = null;
                            return new CloseResources(previous);
                        });
        if (start == null) {
            return;
        }
        if (start.cancelledIdleCheck() != null) {
            start.cancelledIdleCheck().cancel(false);
        }
        backendSpot.closeInstanceSpot();
        closeActiveRouteReceives();
        context.closeResources();
    }

    @Override
    public void close() {
        close(ZLinkSpotCloseReason.EXPLICIT_CLOSE, Instant.now());
    }

    private record IdleSchedule(ScheduledFuture<?> previous, long delayNanos) {}

    private record CloseStart(boolean retryIdle, ScheduledFuture<?> cancelledIdleCheck) {}

    private record CloseResources(ScheduledFuture<?> cancelledIdleCheck) {}
}
