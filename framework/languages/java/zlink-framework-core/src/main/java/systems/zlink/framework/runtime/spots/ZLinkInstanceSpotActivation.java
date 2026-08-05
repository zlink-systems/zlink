package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.time.Instant;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Supplier;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;

final class ZLinkInstanceSpotActivation
    extends SpotActivationBase<DefaultInstanceSpotContext> {
    private final ZLinkInstanceSpot spot;
    private final AtomicBoolean resourcesClosed = new AtomicBoolean();
    private final Object idleLock = new Object();
    private volatile CompletionStage<Boolean> closeFuture;
    private volatile ScheduledFuture<?> idleCheck;
    private volatile long idleTimeoutNanos;
    private volatile long lastActivityNanos = System.nanoTime();
    private volatile boolean closeStarted;
    private volatile String expectedOwnerId;
    private volatile long expectedOwnerLeaseGeneration = -1;
    private volatile long expectedAuthorityOwnerGeneration = -1;
    private volatile long expectedNodeGeneration = -1;
    private volatile String sealedStoreVersion;

    ZLinkInstanceSpotActivation(
        ZLinkSpotRuntime host,
        ZLinkSpotHandlerInvoker handlerInvoker,
        ZLinkInstanceSpot spot,
        ZLinkBackendSpot backendSpot,
        DefaultInstanceSpotContext context) {
        super(host, handlerInvoker, spot, backendSpot, context);
        this.spot = spot;
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        if (host.isClosing()
            || closeStarted
            || info.event() != ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            return CompletableFuture.completedFuture(null);
        }
        // The route drain only reads frames and submits each payload after its
        // header has been admitted. Wrapping the whole drain in the same
        // queue would make the payload admission wait behind its own active
        // turn.
        CompletionStage<Void> dispatch = drainRoutes();
        return dispatch.whenComplete((ignored, failure) -> {
            if (failure == null) {
                lastActivityNanos = System.nanoTime();
            }
        });
    }

    void setAuthorityFence(
        String ownerId,
        long ownerLeaseGeneration,
        long authorityOwnerGeneration,
        long nodeGeneration) {
        expectedOwnerId = ownerId;
        expectedOwnerLeaseGeneration = ownerLeaseGeneration;
        expectedAuthorityOwnerGeneration = authorityOwnerGeneration;
        expectedNodeGeneration = nodeGeneration;
    }

    boolean authorityFenceMatches(
        String ownerId,
        long ownerLeaseGeneration,
        long authorityOwnerGeneration) {
        return (expectedOwnerId == null || expectedOwnerId.equals(ownerId))
            && (expectedOwnerLeaseGeneration < 0
                || expectedOwnerLeaseGeneration == ownerLeaseGeneration)
            && (expectedAuthorityOwnerGeneration < 0
                || expectedAuthorityOwnerGeneration == authorityOwnerGeneration);
    }

    void markSealedStoreVersion(String storeVersion) {
        sealedStoreVersion = storeVersion;
    }

    String sealedStoreVersion() {
        return sealedStoreVersion;
    }

    long expectedNodeGeneration() {
        return expectedNodeGeneration;
    }

    systems.zlink.framework.runtime.internal.service
        .ZLinkServiceM6BWireCodec.InstanceRouteFence authorityRouteFence() {
        return new systems.zlink.framework.runtime.internal.service
            .ZLinkServiceM6BWireCodec.InstanceRouteFence(
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
        idleTimeoutNanos = Math.max(1L, nanos);
        synchronized (idleLock) {
            scheduleIdleCheckLocked(idleTimeoutNanos);
        }
    }

    private void scheduleIdleCheckLocked(long delayNanos) {
        if (idleTimeoutNanos <= 0 || closeStarted || resourcesClosed.get()) {
            return;
        }
        if (idleCheck != null) {
            idleCheck.cancel(false);
        }
        idleCheck = host.scheduleInstanceSpotIdleCheck(
            this::idleTimerFired,
            Math.max(1L, delayNanos));
    }

    private void idleTimerFired() {
        synchronized (idleLock) {
            idleCheck = null;
        }
        if (!isIdleCandidate()) {
            rescheduleIdleCheck();
            return;
        }
        context.awaitQuiescence().whenComplete((ignored, failure) -> {
            if (failure != null || !isIdleCandidate()) {
                rescheduleIdleCheck();
                return;
            }
            closeWithReason(ZLinkSpotCloseReason.IDLE_EVICTED);
        });
    }

    private boolean isIdleCandidate() {
        long timeout = idleTimeoutNanos;
        return timeout > 0
            && !host.isClosing()
            && !host.isRelocating()
            && !closeStarted
            && System.nanoTime() - lastActivityNanos >= timeout
            && !hasActiveRouteReceives()
            && !context.hasActiveTimers();
    }

    private void rescheduleIdleCheck() {
        synchronized (idleLock) {
            if (idleTimeoutNanos > 0 && !closeStarted) {
                scheduleIdleCheckLocked(idleTimeoutNanos);
            }
        }
    }

    private CompletionStage<Void> drainRoutes() {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            ZLinkBackendReceived received =
                backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
            if (received == null) {
                return tail;
            }
            batch.record(ZLinkReceiveBatchBudget.bytesOf(
                received.parts(),
                received.applicationMetadataSize(),
                received.acceptedJournalRecordSize()));
            trackRouteReceived(received);
            ParsedPacket packet;
            try {
                packet = ZLinkSpotRuntime.parsePacket(received.parts());
            } catch (RuntimeException invalid) {
                closeRouteReceived(received);
                continue;
            }
            tail = tail.thenCompose(
                ignored -> dispatchSpotRouteHandler(received, packet));
        }
        return tail;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
        CompletionStage<Void> tail,
        Supplier<CompletionStage<Void>> operation) {
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
        return CompletableFuture.failedFuture(new IllegalStateException(
            "Instance Spot does not own Actor lifecycle"));
    }

    void close(ZLinkSpotCloseReason reason, Instant deadline) {
        try {
            host.awaitClosing(context.runLifecycle(() -> spot.onClosing(
                new ZLinkSpotClosingContext(reason, deadline))));
        } finally {
            closeResources();
        }
    }

    synchronized CompletionStage<Boolean> closeExplicit() {
        return closeWithReason(ZLinkSpotCloseReason.EXPLICIT_CLOSE);
    }

    private synchronized CompletionStage<Boolean> closeWithReason(
        ZLinkSpotCloseReason reason) {
        if (closeFuture != null) {
            return closeFuture;
        }
        if (reason == ZLinkSpotCloseReason.IDLE_EVICTED
            && (!isIdleCandidate()
                || context.hasActiveTimers()
                || hasActiveRouteReceives())) {
            rescheduleIdleCheck();
            return CompletableFuture.completedFuture(false);
        }
        closeStarted = true;
        synchronized (idleLock) {
            if (idleCheck != null) {
                idleCheck.cancel(false);
                idleCheck = null;
            }
        }
        CompletableFuture<Boolean> result = new CompletableFuture<>();
        closeFuture = result;
        CompletionStage<Boolean> seal;
        try {
            seal = host.sealInstanceSpotAuthority(this);
        } catch (RuntimeException failure) {
            seal = CompletableFuture.failedFuture(failure);
        }
        seal
            .thenCompose(sealed -> {
                if (!sealed) {
                    closeResources();
                    host.discardInstanceSpotActivation(this);
                    return CompletableFuture.completedFuture(false);
                }
                CompletionStage<Throwable> callback;
                try {
                    callback = context.runLifecycleExecution(() -> spot.onClosing(
                            new ZLinkSpotClosingContext(reason, Instant.now())))
                        .handle((ignored, failure) -> failure);
                } catch (RuntimeException failure) {
                    callback = CompletableFuture.completedFuture(failure);
                }
                return callback.thenCompose(failure ->
                    host.completeInstanceSpotClose(this)
                        .thenApply(closed -> {
                            closeResources();
                            if (failure != null) {
                                throw new java.util.concurrent.CompletionException(
                                    failure);
                            }
                            return closed;
                        }));
            })
            .whenComplete((closed, failure) -> {
                if (failure != null) {
                    synchronized (this) {
                        if (closeFuture == result) {
                            closeFuture = null;
                            closeStarted = false;
                        }
                    }
                    if (reason == ZLinkSpotCloseReason.IDLE_EVICTED) {
                        rescheduleIdleCheck();
                    }
                    result.completeExceptionally(failure);
                    return;
                }
                result.complete(closed);
            });
        return result;
    }

    void closeResources() {
        if (!resourcesClosed.compareAndSet(false, true)) {
            return;
        }
        closeStarted = true;
        synchronized (idleLock) {
            if (idleCheck != null) {
                idleCheck.cancel(false);
                idleCheck = null;
            }
        }
        backendSpot.closeInstanceSpot();
        closeActiveRouteReceives();
        context.closeResources();
    }

    @Override
    public void close() {
        closeStarted = true;
        close(ZLinkSpotCloseReason.EXPLICIT_CLOSE, Instant.now());
    }
}
