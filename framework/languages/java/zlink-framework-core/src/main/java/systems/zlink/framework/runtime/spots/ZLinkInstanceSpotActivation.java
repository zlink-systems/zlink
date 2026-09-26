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
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledFuture;
import java.util.function.Supplier;

final class ZLinkInstanceSpotActivation extends SpotActivationBase<DefaultInstanceSpotContext> {
    private final ZLinkInstanceSpot spot;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private boolean resourcesClosed;
    private CompletionStage<Boolean> closeFuture;
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
                            if (idleTimeoutNanos <= 0 || closeFuture != null || resourcesClosed) {
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
        if (idleTimeoutNanos <= 0 || closeFuture != null || resourcesClosed) {
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
                && closeFuture == null
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
                    backendSpot.sealInstanceSpotAdmission();
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
        CloseStart start =
                inStateLane(
                        () -> {
                            if (closeFuture != null) {
                                return new CloseStart(closeFuture, null, false, null);
                            }
                            boolean retryIdle =
                                    reason == ZLinkSpotCloseReason.IDLE_EVICTED
                                            && (!isIdleCandidateOnLane()
                                                    || context.hasActiveTimers()
                                                    || hasActiveRouteReceives());
                            if (retryIdle) {
                                return new CloseStart(null, null, true, null);
                            }
                            ScheduledFuture<?> previous = idleCheck;
                            idleCheck = null;
                            CompletableFuture<Boolean> result = new CompletableFuture<>();
                            closeFuture = result;
                            return new CloseStart(null, result, false, previous);
                        });
        if (start.existing() != null) {
            return start.existing();
        }
        if (start.cancelledIdleCheck() != null) {
            start.cancelledIdleCheck().cancel(false);
        }
        if (start.retryIdle()) {
            rescheduleIdleCheck();
            return CompletableFuture.completedFuture(false);
        }
        inStateLane(
                () -> {
                    backendSpot.sealInstanceSpotAdmission();
                    drainRoutes();
                    return null;
                });
        CompletableFuture<Boolean> result = start.result();
        CompletionStage<Boolean> seal;
        try {
            seal = host.sealInstanceSpotAuthority(this);
        } catch (RuntimeException failure) {
            seal = CompletableFuture.failedFuture(failure);
        }
        seal.thenCompose(
                        sealedResult -> {
                            if (!sealedResult) {
                                closeResources();
                                host.discardInstanceSpotActivation(this);
                                return CompletableFuture.completedFuture(false);
                            }
                            return host.admitNewApplicationJob(
                                            () -> CompletableFuture.completedFuture(null))
                                    .thenCompose(
                                            ignored -> {
                                                try {
                                                    return closingCallback(
                                                                    () ->
                                                                            context.runClosing(
                                                                                    () ->
                                                                                            context
                                                                                                    .runLifecycleExecution(
                                                                                                            () ->
                                                                                                                    spot
                                                                                                                            .onClosing(
                                                                                                                                    new ZLinkSpotClosingContext(
                                                                                                                                            reason,
                                                                                                                                            Instant
                                                                                                                                                    .now())))))
                                                            .handle((done, failure) -> failure);
                                                } catch (RuntimeException failure) {
                                                    return CompletableFuture.completedFuture(
                                                            failure);
                                                }
                                            })
                                    .thenCompose(
                                            failure ->
                                                    host.completeInstanceSpotClose(this)
                                                            .thenApply(
                                                                    closed -> {
                                                                        closeResources();
                                                                        if (failure != null) {
                                                                            throw new CompletionException(
                                                                                    failure);
                                                                        }
                                                                        return closed;
                                                                    }));
                        })
                .whenComplete(
                        (closed, failure) -> {
                            if (failure != null) {
                                inStateLane(
                                        () -> {
                                            if (closeFuture == result) {
                                                closeFuture = null;
                                                backendSpot.restoreInstanceSpotAdmission();
                                            }
                                            return null;
                                        });
                                if (reason == ZLinkSpotCloseReason.IDLE_EVICTED) {
                                    rescheduleIdleCheck();
                                }
                                result.completeAsync(
                                        () -> {
                                            throw new CompletionException(failure);
                                        });
                                return;
                            }
                            result.completeAsync(() -> closed);
                        });
        return result;
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

    private record CloseStart(
            CompletionStage<Boolean> existing,
            CompletableFuture<Boolean> result,
            boolean retryIdle,
            ScheduledFuture<?> cancelledIdleCheck) {}

    private record CloseResources(ScheduledFuture<?> cancelledIdleCheck) {}
}
