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
            CompletionStage<Void> dispatched = admitRoute(received);
            tail = tail.thenCombine(dispatched, (ignored, done) -> null);
        }
        return tail;
    }

    CompletionStage<Void> admitRoute(ZLinkBackendReceived received) {
        return admitRoute(received, null);
    }

    CompletionStage<Void> admitExisting(
            systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec
                            .InstanceSpotMessage
                    message,
            ZLinkBackendReceived received) {
        var route = message.route();
        if (!context.nodeRid().equals(route.targetNodeRid())
                || !authorityFenceMatches(
                        route.ownerId(), route.leaseGeneration(), route.authorityOwnerGeneration())
                || (hasExpectedNodeGeneration()
                        && expectedNodeGeneration() != route.targetNodeGeneration())) {
            received.close();
            return CompletableFuture.failedFuture(
                    systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin.framework(
                            systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                            "Instance Spot owner fence changed"));
        }
        return admitRoute(received);
    }

    CompletionStage<Void> admitRoute(
            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        var hostRejection = host.spotHostAdmissionFailure(context.spotId());
        if (hostRejection != null) {
            received.close();
            if (admission != null) admission.completeExceptionally(hostRejection);
            return CompletableFuture.failedFuture(hostRejection);
        }
        trackRouteReceived(received);
        CompletionStage<Void> admitted;
        try {
            ParsedPacket observed = null;
            systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.State flow = null;
            if (host.flowCaptureEnabled()) {
                try {
                    observed = ZLinkSpotRuntime.parsePacket(received.parts());
                    flow =
                            observed.header() == null
                                    ? ZLinkSpotFlowFrame.decode(received.parts())
                                    : ZLinkSpotFlowFrame.fromEnvelopeHeader(observed.header());
                } catch (systems.zlink.framework.errors.ZLinkFrameworkException invalidEnvelope) {
                    failRouteInvalidFlow(received, invalidEnvelope);
                    if (admission != null) admission.complete(null);
                    return CompletableFuture.completedFuture(null);
                }
            }
            admitted =
                    context.enqueueMessage(
                            received,
                            () -> {
                                ParsedPacket packet;
                                try {
                                    packet = ZLinkSpotRuntime.parsePacket(received.parts());
                                } catch (
                                        systems.zlink.framework.errors.ZLinkFrameworkException
                                                invalidEnvelope) {
                                    failRouteInvalidFlow(received, invalidEnvelope);
                                    return CompletableFuture.completedFuture(null);
                                } catch (RuntimeException failure) {
                                    closeRouteReceived(received);
                                    return CompletableFuture.failedFuture(failure);
                                }
                                return dispatchSpotRouteHandler(received, packet);
                            },
                            admission);
            if (observed != null) {
                try (var ignored =
                        systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext
                                .enterOrCreate(
                                        flow,
                                        systems.zlink.framework.monitoring.ZLinkFlowOrigin
                                                .INBOUND)) {
                    host.traceSpotRouteFlow(
                            systems.zlink.framework.runtime.internal.diagnostics
                                    .ZLinkMessageFlowOutcome.RECEIVED,
                            received.isRequest()
                                    ? systems.zlink.framework.runtime.internal.diagnostics
                                            .ZLinkDispatchMessageKind.REQUEST
                                    : systems.zlink.framework.runtime.internal.diagnostics
                                            .ZLinkDispatchMessageKind.SEND,
                            observed.packetName(),
                            received.requestSeq(),
                            context.spotId());
                }
            }
        } catch (RuntimeException | Error failure) {
            if (admission != null) admission.completeExceptionally(failure);
            closeRouteReceived(received);
            throw failure;
        }
        return admitted.whenComplete(
                (done, failure) -> {
                    if (failure != null) {
                        closeRouteReceived(received);
                    }
                });
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
        return tail.thenCompose(ignored -> operation.get());
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

    CompletionStage<Void> closeAsync(ZLinkSpotCloseReason reason, Instant deadline) {
        return onStateLane(
                        () -> {
                            context.sealClosingAdmission();
                            drainRoutes();
                            return null;
                        })
                .thenCompose(ignored -> closingStage(reason, deadline))
                .handle((ignored, failure) -> finishCleanup(failure, closeResourcesAsync()))
                .thenCompose(stage -> stage);
    }

    CompletionStage<Void> closingStage(ZLinkSpotCloseReason reason, Instant deadline) {
        return closingCallback(
                () ->
                        context.runClosing(
                                () ->
                                        spot.onClosing(
                                                new ZLinkSpotClosingContext(reason, deadline))));
    }

    CompletionStage<Boolean> closeExplicit() {
        return closeWithReason(ZLinkSpotCloseReason.EXPLICIT_CLOSE);
    }

    private CompletionStage<Boolean> closeWithReason(ZLinkSpotCloseReason reason) {
        CompletableFuture<Boolean> result = new CompletableFuture<>();
        return context.enqueueClose(
                        () -> closeOnLifecycle(reason).thenAccept(result::complete),
                        message ->
                                host.replayClosedInstanceMessage(
                                        this, (ZLinkBackendReceived) message),
                        () ->
                                existingCloseCoordinator() != null
                                        && existingCloseCoordinator().committed())
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure != null) result.completeExceptionally(failure);
                        })
                .thenCompose(ignored -> result);
    }

    private CompletionStage<Boolean> closeOnLifecycle(ZLinkSpotCloseReason reason) {
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
                                                            context.ownerQueue()
                                                                    .commitLifecycleTransition();
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
                                                                                                spot
                                                                                                        .onClosing(
                                                                                                                new ZLinkSpotClosingContext(
                                                                                                                        reason,
                                                                                                                        Instant
                                                                                                                                .now()))))),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            backendSpot.closeInstanceSpot();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        () -> {
                                                            context.ownerQueue()
                                                                    .pendingMessages()
                                                                    .forEach(
                                                                            message ->
                                                                                    transferRouteReceived(
                                                                                            (ZLinkBackendReceived)
                                                                                                    message));
                                                            closeActiveRouteReceives();
                                                            return CompletableFuture
                                                                    .completedFuture(null);
                                                        }),
                                                ZLinkSpotCloseCoordinator.Step.operation(
                                                        context::closeTimersAsync),
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
                                                                                                "Instance"
                                                                                                        + " Spot"
                                                                                                        + " authority"
                                                                                                        + " changed"
                                                                                                        + " during"
                                                                                                        + " Close");
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

    CompletionStage<Void> replayMessage(ZLinkBackendReceived received) {
        trackRouteReceived(received);
        return context.runLifecycleExecution(
                () ->
                        dispatchSpotRouteHandler(
                                received, ZLinkSpotRuntime.parsePacket(received.parts())));
    }

    CompletionStage<Void> closeResourcesAsync() {
        return onStateLane(
                        () -> {
                            if (resourcesClosed) {
                                return null;
                            }
                            resourcesClosed = true;
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = null;
                            return new CloseResources(previous);
                        })
                .thenCompose(
                        start -> {
                            if (start == null) {
                                return CompletableFuture.completedFuture(null);
                            }
                            if (start.cancelledIdleCheck() != null) {
                                start.cancelledIdleCheck().cancel(false);
                            }
                            backendSpot.closeInstanceSpot();
                            closeActiveRouteReceives();
                            return context.closeResourcesAsync();
                        });
    }

    private record IdleSchedule(ScheduledFuture<?> previous, long delayNanos) {}

    private record CloseStart(boolean retryIdle, ScheduledFuture<?> cancelledIdleCheck) {}

    private record CloseResources(ScheduledFuture<?> cancelledIdleCheck) {}
}
