package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;

public final class ZLinkLocationRuntime implements AutoCloseable {
    private final ZLinkRegisteredLocationStores stores;
    private final String ownerId;
    private final Duration ownerLeaseTtl;
    private final Duration heartbeatInterval;
    private final Duration ownerLeaseRenewTimeout;
    private final Duration ownerLeaseFencingMargin;
    private final ScheduledExecutorService heartbeatExecutor;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private ScheduledFuture<?> heartbeatTask;
    private StartupLifecycle startupLifecycle;
    private RoutingId nodeRid;
    private boolean started;
    private long ownerAdmissionDeadlineNanos;
    private String lastError;
    private Instant ownerLeaseRenewedAt;
    private ZLinkLocationOwnerToken ownerToken;
    private ZLinkLocationOwnerToken recoveryPreviousOwnerToken;
    private long nextOwnerLeaseRenewalNanos;
    private Supplier<CompletionStage<Void>> ownerLeaseRecoveryListener;

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

    ZLinkLocationRuntime(
            ZLinkLocationRepository store, Duration ownerLeaseTtl, Duration heartbeatInterval) {
        this(
                ZLinkRegisteredLocationStores.fromUnified(store),
                UUID.randomUUID().toString().replace("-", ""),
                ownerLeaseTtl,
                heartbeatInterval,
                defaultOwnerLeaseRenewTimeout(),
                defaultOwnerLeaseFencingMargin());
    }

    ZLinkLocationRuntime(
            ZLinkLocationRepository store,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval) {
        this(
                ZLinkRegisteredLocationStores.fromUnified(store),
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                defaultOwnerLeaseRenewTimeout(),
                defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval) {
        this(
                stores,
                UUID.randomUUID().toString().replace("-", ""),
                ownerLeaseTtl,
                heartbeatInterval,
                defaultOwnerLeaseRenewTimeout(),
                defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout) {
        this(
                stores,
                UUID.randomUUID().toString().replace("-", ""),
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin) {
        this(
                stores,
                UUID.randomUUID().toString().replace("-", ""),
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                ownerLeaseFencingMargin);
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval) {
        this(
                stores,
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                defaultOwnerLeaseRenewTimeout(),
                defaultOwnerLeaseFencingMargin());
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout) {
        this(
                stores,
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                defaultOwnerLeaseFencingMargin());
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.ownerId = requireText(ownerId, "ownerId");
        this.ownerLeaseTtl = requirePositive(ownerLeaseTtl, "ownerLeaseTtl");
        this.heartbeatInterval = requirePositive(heartbeatInterval, "heartbeatInterval");
        this.ownerLeaseRenewTimeout =
                requirePositive(ownerLeaseRenewTimeout, "ownerLeaseRenewTimeout");
        this.ownerLeaseFencingMargin =
                requireNonNegative(ownerLeaseFencingMargin, "ownerLeaseFencingMargin");
        this.heartbeatExecutor =
                Executors.newSingleThreadScheduledExecutor(
                        task -> {
                            Thread thread = new Thread(task, "zlink-location-owner-lease");
                            thread.setDaemon(true);
                            return thread;
                        });
    }

    public String ownerId() {
        return ownerId;
    }

    ZLinkLocationOwnerToken ownerTokenSnapshot() {
        return inStateLane(
                () -> {
                    ensureOwnerAdmissionOpenCore();
                    return ownerToken;
                });
    }

    public ZLinkLocationOwnerToken currentOwnerToken() {
        return ownerTokenSnapshot();
    }

    /**
     * The owner token a new Store change may carry. It is empty once the local admission deadline
     * has passed: accepted work may still finish and clean up, but an expired owner makes no new
     * Store change (Location runtime §5).
     */
    public Optional<ZLinkLocationOwnerToken> admittedOwnerToken() {
        return inStateLane(this::admittedOwnerTokenCore);
    }

    public ZLinkLocationOwnerToken recoveryPreviousOwnerToken() {
        return inStateLane(() -> recoveryPreviousOwnerToken);
    }

    /**
     * Registers the runtime action that republishes owner-scoped records after a stale lease is
     * replaced by a new generation.
     */
    public void setOwnerLeaseRecoveryListener(Supplier<CompletionStage<Void>> listener) {
        inStateLane(
                () -> {
                    ownerLeaseRecoveryListener = listener;
                    return null;
                });
    }

    ZLinkLocationRepository locationStore() {
        return stores.unifiedStore();
    }

    public boolean ownerLeaseHealthy() {
        return isOwnerAdmissionOpen();
    }

    public boolean isOwnerAdmissionOpen() {
        return inStateLane(this::isOwnerAdmissionOpenCore);
    }

    public void ensureOwnerAdmissionOpen() {
        inStateLane(
                () -> {
                    ensureOwnerAdmissionOpenCore();
                    return null;
                });
    }

    public String lastError() {
        return inStateLane(() -> lastError);
    }

    public Instant ownerLeaseRenewedAt() {
        return inStateLane(() -> ownerLeaseRenewedAt);
    }

    public CompletionStage<Void> start(RoutingId nodeRid) {
        Instant cleanupDeadline = Instant.now().plus(ownerLeaseRenewTimeout);
        return start(nodeRid, () -> cleanupDeadline);
    }

    public CompletionStage<Void> start(RoutingId nodeRid, Supplier<Instant> cleanupDeadline) {
        Objects.requireNonNull(cleanupDeadline, "cleanupDeadline");
        Objects.requireNonNull(nodeRid, "nodeRid");
        StartState state =
                inStateLane(
                        () -> {
                            if (started) {
                                return new StartState(startupLifecycle, false);
                            }
                            started = true;
                            this.nodeRid = nodeRid;
                            startupLifecycle =
                                    new StartupLifecycle(
                                            new CompletableFuture<>(),
                                            new CompletableFuture<>(),
                                            cleanupDeadline);
                            return new StartState(startupLifecycle, true);
                        });
        if (state.claim()) {
            attemptInitialOwnerLeaseClaim(state.lifecycle());
        }
        return state.lifecycle().completion();
    }

    public void cancelStartup() {
        cancelStartup(inStateLane(this::startupCompletion));
    }

    private static void cancelStartup(CompletableFuture<Void> completion) {
        if (completion != null) completion.cancel(false);
    }

    public CompletionStage<Void> stop() {
        return stop(Instant.now().plus(ownerLeaseRenewTimeout));
    }

    public CompletionStage<Void> stop(Instant deadline) {
        Objects.requireNonNull(deadline, "deadline");
        StopState state =
                inStateLane(
                        () -> {
                            boolean shouldStop = started;
                            started = false;
                            ScheduledFuture<?> heartbeat = heartbeatTask;
                            heartbeatTask = null;
                            StartupLifecycle lifecycle = startupLifecycle;
                            CompletableFuture<Void> completion =
                                    lifecycle == null ? null : lifecycle.cleanup();
                            ZLinkLocationOwnerToken token =
                                    shouldStop && startupCompletion().isDone()
                                            ? admittedOwnerTokenCore().orElse(null)
                                            : null;
                            if (shouldStop) {
                                completion = new CompletableFuture<>();
                                startupLifecycle =
                                        new StartupLifecycle(
                                                lifecycle.completion(),
                                                completion,
                                                lifecycle.completion().isDone()
                                                                && !lifecycle
                                                                        .completion()
                                                                        .isCompletedExceptionally()
                                                        ? () -> deadline
                                                        : lifecycle.cleanupDeadline());
                            }
                            return new StopState(
                                    shouldStop, heartbeat, token, lifecycle, completion);
                        });
        cancel(state.heartbeat());
        if (state.lifecycle() == null) return CompletableFuture.completedFuture(null);
        cancelStartup(state.lifecycle().completion());
        if (!state.shouldStop()) return state.completion();
        long deadlineNanos = deadlineNanos(deadline);
        ZLinkLocationOwnerToken token = state.token();
        CompletionStage<Void> cleanup =
                withinDeadline(state.lifecycle().cleanup(), deadlineNanos)
                        .handle((ignored, failure) -> failure)
                        .thenCompose(
                                pendingFailure ->
                                        (token == null
                                                        ? CompletableFuture.completedFuture(0L)
                                                        : withinDeadline(
                                                                () ->
                                                                        stores.unifiedStore()
                                                                                .removeAllByOwner(
                                                                                        token),
                                                                deadlineNanos))
                                                .thenCompose(
                                                        ignored ->
                                                                token == null
                                                                        ? CompletableFuture
                                                                                .completedFuture(
                                                                                        null)
                                                                        : withinDeadline(
                                                                                        () ->
                                                                                                stores.ownerLeaseStore()
                                                                                                        .releaseOwnerLease(
                                                                                                                token),
                                                                                        deadlineNanos)
                                                                                .thenApply(
                                                                                        released ->
                                                                                                null))
                                                .thenCompose(
                                                        ignored ->
                                                                stateLane.runAsync(
                                                                        () -> {
                                                                            if (ownsStartupCore(
                                                                                    state.lifecycle()
                                                                                            .completion())) {
                                                                                ownerToken = null;
                                                                                ownerAdmissionDeadlineNanos =
                                                                                        0L;
                                                                            }
                                                                            return null;
                                                                        }))
                                                .handle(
                                                        (ignored, failure) -> {
                                                            if (pendingFailure != null) {
                                                                Throwable original =
                                                                        unwrap(pendingFailure);
                                                                if (failure != null)
                                                                    original.addSuppressed(
                                                                            unwrap(failure));
                                                                throw new CompletionException(
                                                                        original);
                                                            }
                                                            if (failure != null)
                                                                throw new CompletionException(
                                                                        unwrap(failure));
                                                            return null;
                                                        }));
        cleanup.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) state.completion().complete(null);
                    else state.completion().completeExceptionally(unwrap(failure));
                });
        return state.completion();
    }

    private CompletableFuture<Void> startupCompletion() {
        return startupLifecycle == null ? null : startupLifecycle.completion();
    }

    private long cleanupDeadlineNanos(StartupLifecycle lifecycle) {
        Supplier<Instant> cleanupDeadline =
                inStateLane(
                        () ->
                                ownsStartupCore(lifecycle.completion())
                                        ? startupLifecycle.cleanupDeadline()
                                        : lifecycle.cleanupDeadline());
        return deadlineNanos(cleanupDeadline.get());
    }

    private static long deadlineNanos(Instant deadline) {
        Duration remaining = Duration.between(Instant.now(), deadline);
        return saturatingAdd(System.nanoTime(), Math.max(0L, remaining.toNanos()));
    }

    public CompletionStage<Boolean> renewOwnerLeaseOnce() {
        return stateLane
                .runAsync(() -> ownerLeaseRenewalCore(startupCompletion()))
                .thenCompose(Supplier::get);
    }

    private Supplier<CompletionStage<Boolean>> ownerLeaseRenewalCore(
            CompletableFuture<Void> lifecycle) {
        nextOwnerLeaseRenewalNanos = System.nanoTime() + heartbeatInterval.toNanos();
        RenewState state = new RenewState(nodeRid, ownerToken, isOwnerAdmissionOpenCore());
        return () -> renewOwnerLease(state, lifecycle);
    }

    private CompletionStage<Boolean> renewOwnerLease(
            RenewState state, CompletableFuture<Void> lifecycle) {
        if (state.nodeRid() == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Location runtime must be started before renewing its owner lease."));
        }
        ZLinkLocationOwnerToken token = state.token();
        if (token == null) {
            return reportRenewalFailure(
                    claimOwnerLease(lifecycle)
                            .thenCompose(ignored -> republishAfterOwnerLeaseRecovery(lifecycle))
                            .thenApply(ignored -> true),
                    lifecycle);
        }
        long operationStartedNanos = System.nanoTime();
        CompletionStage<Boolean> renewal =
                stores.ownerLeaseStore()
                        .renewOwnerLease(token, ownerLeaseTtl)
                        .handle(
                                (result, failure) ->
                                        stateLane.<Supplier<CompletionStage<Boolean>>>runAsync(
                                                () -> {
                                                    if (!started || !token.equals(ownerToken)) {
                                                        return () ->
                                                                CompletableFuture.completedFuture(
                                                                        false);
                                                    }
                                                    if (failure != null) {
                                                        recordFailureCore(failureMessage(failure));
                                                        return () ->
                                                                CompletableFuture.completedFuture(
                                                                        false);
                                                    }
                                                    if (result
                                                            instanceof
                                                            systems.zlink.framework.runtime.internal
                                                                            .locations
                                                                            .ZLinkOwnerLeaseRenewed
                                                                    renewed) {
                                                        recordSuccessfulRenewalCore(
                                                                renewed.leaseExpiresAt(),
                                                                renewed.storeNow(),
                                                                operationStartedNanos);
                                                        nextOwnerLeaseRenewalNanos =
                                                                operationStartedNanos
                                                                        + heartbeatInterval
                                                                                .toNanos();
                                                        return () ->
                                                                state.admissionOpen()
                                                                        ? CompletableFuture
                                                                                .completedFuture(
                                                                                        true)
                                                                        : republishAfterOwnerLeaseRecovery(
                                                                                        lifecycle)
                                                                                .thenApply(
                                                                                        ignored ->
                                                                                                true);
                                                    }
                                                    recordFailureCore(
                                                            "owner lease renewal was stale");
                                                    ownerAdmissionDeadlineNanos = 0L;
                                                    return () ->
                                                            claimOwnerLease(lifecycle)
                                                                    .thenCompose(
                                                                            ignored ->
                                                                                    republishAfterOwnerLeaseRecovery(
                                                                                            lifecycle))
                                                                    .thenApply(ignored -> true);
                                                }))
                        .thenCompose(stage -> stage)
                        .thenCompose(Supplier::get);
        return reportRenewalFailure(renewal, lifecycle);
    }

    /** Records renewal failures only for the lifecycle that requested them. */
    private CompletionStage<Boolean> reportRenewalFailure(
            CompletionStage<Boolean> renewal, CompletableFuture<Void> lifecycle) {
        return renewal.exceptionallyCompose(
                failure ->
                        stateLane.runAsync(
                                () -> {
                                    if (started && ownsStartupCore(lifecycle)) {
                                        recordFailureCore(failureMessage(failure));
                                    }
                                    return false;
                                }));
    }

    private boolean ownsStartupCore(CompletableFuture<Void> completion) {
        return startupCompletion() == completion;
    }

    private CompletionStage<Void> claimOwnerLease(CompletableFuture<Void> lifecycle) {
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        return resolveOwnerLeaseClaim(
                        deadlineNanos,
                        stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl))
                .thenCompose(
                        claimed -> installOwnerLease(claimed, operationStartedNanos, lifecycle));
    }

    private CompletionStage<ZLinkOwnerLeaseClaimed> resolveOwnerLeaseClaim(
            long deadlineNanos, CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation) {
        return withinDeadline(claimOperation, deadlineNanos)
                .<CompletionStage<ZLinkOwnerLeaseClaimResult>>handle(
                        (result, failure) -> {
                            if (failure == null) {
                                return CompletableFuture.completedFuture(result);
                            }
                            return confirmClaimAfterFailure(failure, deadlineNanos);
                        })
                .thenCompose(result -> result)
                .thenCompose(
                        result ->
                                result
                                                instanceof
                                                systems.zlink.framework.runtime.internal.locations
                                                        .ZLinkOwnerLeaseClaimConflict
                                        ? confirmClaimAfterConflict(result, deadlineNanos)
                                        : CompletableFuture.completedFuture(result))
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkOwnerLeaseClaimed claimed) {
                                return CompletableFuture.completedFuture(claimed);
                            }
                            return CompletableFuture.failedFuture(
                                    new OwnerLeaseClaimRejectedException(
                                            result instanceof ZLinkOwnerLeaseGenerationExhausted
                                                    ? "owner lease generation is exhausted"
                                                    : "owner lease is already claimed"));
                        });
    }

    private CompletionStage<Void> installOwnerLease(
            ZLinkOwnerLeaseClaimed claimed,
            long operationStartedNanos,
            CompletableFuture<Void> lifecycle) {
        return stateLane.runAsync(
                () -> {
                    if (!started || !ownsStartupCore(lifecycle)) {
                        throw new CancellationException("owner lease lifecycle ended");
                    }
                    recoveryPreviousOwnerToken = ownerToken;
                    installOwnerLeaseCore(claimed, operationStartedNanos);
                    return null;
                });
    }

    private void installOwnerLeaseCore(ZLinkOwnerLeaseClaimed claimed, long operationStartedNanos) {
        ownerToken = claimed.token();
        recordSuccessfulRenewalCore(
                claimed.leaseExpiresAt(), claimed.storeNow(), operationStartedNanos);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterConflict(
            ZLinkOwnerLeaseClaimResult conflict, long deadlineNanos) {
        return withinDeadline(() -> stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
                .handle(
                        (result, failure) ->
                                failure == null
                                                && result
                                                        instanceof
                                                        systems.zlink.framework.runtime.internal
                                                                        .locations
                                                                        .ZLinkOwnerLeaseFound
                                                                found
                                                && ownerId.equals(found.token().ownerId())
                                        ? new ZLinkOwnerLeaseClaimed(
                                                found.token(),
                                                found.leaseExpiresAt(),
                                                found.storeNow())
                                        : conflict);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterFailure(
            Throwable failure, long deadlineNanos) {
        return withinDeadline(() -> stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
                .thenCompose(
                        result ->
                                result
                                                        instanceof
                                                        systems.zlink.framework.runtime.internal
                                                                        .locations
                                                                        .ZLinkOwnerLeaseFound
                                                                found
                                                && ownerId.equals(found.token().ownerId())
                                        ? CompletableFuture.completedFuture(
                                                new ZLinkOwnerLeaseClaimed(
                                                        found.token(),
                                                        found.leaseExpiresAt(),
                                                        found.storeNow()))
                                        : CompletableFuture.failedFuture(failure));
    }

    private <T> CompletionStage<T> withinDeadline(
            CompletionStage<T> operation, long deadlineNanos) {
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0L) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("owner lease operation timed out"));
        }
        CompletableFuture<T> completion = new CompletableFuture<>();
        ScheduledFuture<?> timeout =
                heartbeatExecutor.schedule(
                        () ->
                                completion.completeExceptionally(
                                        new TimeoutException("owner lease operation timed out")),
                        remainingNanos,
                        TimeUnit.NANOSECONDS);
        operation.whenComplete(
                (result, failure) -> {
                    if (failure == null && completion.complete(result)) {
                        timeout.cancel(false);
                    } else if (failure != null) {
                        completion.completeExceptionally(failure);
                        timeout.cancel(false);
                    }
                });
        return completion;
    }

    private <T> CompletionStage<T> withinDeadline(
            Supplier<CompletionStage<T>> operation, long deadlineNanos) {
        if (deadlineNanos - System.nanoTime() <= 0L) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("owner lease operation timed out"));
        }
        return withinDeadline(operation.get(), deadlineNanos);
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery(
            CompletableFuture<Void> lifecycle) {
        return stateLane
                .runAsync(
                        () ->
                                started && ownsStartupCore(lifecycle)
                                        ? ownerLeaseRecoveryListener
                                        : null)
                .thenCompose(
                        listener ->
                                listener == null
                                        ? CompletableFuture.<Void>completedFuture(null)
                                        : listener.get()
                                                .exceptionallyCompose(
                                                        failure ->
                                                                closeAdmissionAfterRepublishFailure(
                                                                        failure, lifecycle)));
    }

    private CompletionStage<Void> closeAdmissionAfterRepublishFailure(
            Throwable failure, CompletableFuture<Void> lifecycle) {
        return stateLane
                .runAsync(
                        () -> {
                            if (started && ownsStartupCore(lifecycle)) {
                                ownerAdmissionDeadlineNanos = 0L;
                            }
                            return null;
                        })
                .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
    }

    private void attemptInitialOwnerLeaseClaim(StartupLifecycle lifecycle) {
        CompletableFuture<Void> completion = lifecycle.completion();
        boolean shouldClaim =
                inStateLane(() -> started && ownsStartupCore(completion) && !completion.isDone());
        if (!shouldClaim) {
            lifecycle.cleanup().complete(null);
            return;
        }
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation =
                stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl);
        CompletionStage<ZLinkLocationOwnerToken> terminalClaim =
                claimOperation.handle(
                        (result, failure) ->
                                result instanceof ZLinkOwnerLeaseClaimed claimed
                                        ? claimed.token()
                                        : null);
        completion
                .handle(
                        (ignored, failure) -> {
                            if (failure != null
                                    && unwrap(failure) instanceof CancellationException) {
                                cancelStartupClaim(completion);
                                return cleanupDeadlineNanos(lifecycle);
                            }
                            return null;
                        })
                .thenCompose(
                        cleanupDeadline ->
                                (cleanupDeadline == null
                                                ? terminalClaim
                                                : withinDeadline(terminalClaim, cleanupDeadline))
                                        .thenCompose(
                                                claimedToken ->
                                                        stateLane
                                                                .runAsync(
                                                                        () ->
                                                                                completion
                                                                                                .isCancelled()
                                                                                        || (!started
                                                                                                && !completion
                                                                                                        .isCompletedExceptionally())
                                                                                        || !ownsStartupCore(
                                                                                                completion))
                                                                .thenCompose(
                                                                        abandoned ->
                                                                                abandoned
                                                                                        ? startupCancellationCleanup(
                                                                                                cleanupDeadline
                                                                                                                == null
                                                                                                        ? cleanupDeadlineNanos(
                                                                                                                lifecycle)
                                                                                                        : cleanupDeadline,
                                                                                                claimedToken,
                                                                                                completion)
                                                                                        : CompletableFuture
                                                                                                .completedFuture(
                                                                                                        null))))
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) {
                                lifecycle.cleanup().complete(null);
                            } else {
                                stateLane
                                        .runAsync(
                                                () -> {
                                                    if (ownsStartupCore(completion))
                                                        recordFailureCore(failureMessage(failure));
                                                    return null;
                                                })
                                        .whenComplete(
                                                (recorded, recordingFailure) ->
                                                        lifecycle
                                                                .cleanup()
                                                                .completeExceptionally(
                                                                        unwrap(failure)));
                            }
                        });
        resolveOwnerLeaseClaim(deadlineNanos, claimOperation)
                .handle(
                        (claimed, failure) ->
                                stateLane
                                        .runAsync(
                                                () ->
                                                        installStartupClaimCore(
                                                                completion,
                                                                claimed,
                                                                failure,
                                                                operationStartedNanos))
                                        .thenCompose(
                                                cancelled ->
                                                        finishStartupClaim(
                                                                completion, cancelled, failure)));
    }

    /**
     * Applies the startup claim result to lane state; returns whether the startup was abandoned.
     */
    private boolean installStartupClaimCore(
            CompletableFuture<Void> completion,
            ZLinkOwnerLeaseClaimed claimed,
            Throwable failure,
            long operationStartedNanos) {
        boolean ownsStartup = ownsStartupCore(completion);
        if (!ownsStartup || !started || completion.isCancelled()) {
            if (ownsStartup) {
                ownerToken = null;
                ownerAdmissionDeadlineNanos = 0L;
            }
            return true;
        }
        nextOwnerLeaseRenewalNanos = System.nanoTime() + heartbeatInterval.toNanos();
        if (failure == null) {
            installOwnerLeaseCore(claimed, operationStartedNanos);
            return false;
        }
        recordFailureCore(failureMessage(failure));
        if (unwrap(failure) instanceof OwnerLeaseClaimRejectedException) {
            started = false;
        }
        return false;
    }

    private CompletionStage<Void> finishStartupClaim(
            CompletableFuture<Void> completion, boolean cancelled, Throwable failure) {
        if (cancelled) {
            return CompletableFuture.completedFuture(null);
        }
        if (failure != null && unwrap(failure) instanceof OwnerLeaseClaimRejectedException) {
            completion.completeExceptionally(unwrap(failure));
            return CompletableFuture.completedFuture(null);
        }
        return completeInitialClaim(completion).thenApply(ignored -> null);
    }

    private CompletionStage<Boolean> completeInitialClaim(CompletableFuture<Void> completion) {
        return stateLane
                .runAsync(
                        () -> {
                            if (!started || completion.isCancelled()) {
                                return false;
                            }
                            startHeartbeatCore();
                            return true;
                        })
                .thenApply(running -> running && completion.complete(null));
    }

    /**
     * Schedules the heartbeat once per start. Scheduling does not wait, so it stays in the turn.
     */
    private void startHeartbeatCore() {
        if (!started || (heartbeatTask != null && !heartbeatTask.isDone())) {
            return;
        }
        long delay =
                nextOwnerLeaseRenewalNanos == 0L
                        ? heartbeatInterval.toNanos()
                        : Math.max(0L, nextOwnerLeaseRenewalNanos - System.nanoTime());
        CompletableFuture<Void> lifecycle = startupCompletion();
        heartbeatTask =
                heartbeatExecutor.schedule(
                        () -> renewOwnerLeaseOnHeartbeat(lifecycle), delay, TimeUnit.NANOSECONDS);
    }

    private void cancelStartupClaim(CompletableFuture<Void> completion) {
        ScheduledFuture<?> heartbeat =
                inStateLane(
                        () -> {
                            if (!ownsStartupCore(completion)) {
                                return null;
                            }
                            started = false;
                            ownerToken = null;
                            ownerAdmissionDeadlineNanos = 0L;
                            ScheduledFuture<?> current = heartbeatTask;
                            heartbeatTask = null;
                            return current;
                        });
        cancel(heartbeat);
    }

    private CompletionStage<Void> startupCancellationCleanup(
            long deadlineNanos,
            ZLinkLocationOwnerToken claimedToken,
            CompletableFuture<Void> lifecycle) {
        return withinDeadline(() -> stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
                .thenCompose(
                        result ->
                                stateLane.runAsync(
                                        () -> {
                                            if (result
                                                            instanceof
                                                            systems.zlink.framework.runtime.internal
                                                                            .locations
                                                                            .ZLinkOwnerLeaseFound
                                                                    found
                                                    && ownerId.equals(found.token().ownerId())
                                                    && !found.token().equals(ownerToken)
                                                    && (claimedToken != null
                                                            ? claimedToken.equals(found.token())
                                                            : ownsStartupCore(lifecycle))) {
                                                return found.token();
                                            }
                                            return null;
                                        }))
                .thenCompose(
                        token ->
                                token == null
                                        ? CompletableFuture.completedFuture(null)
                                        : withinDeadline(
                                                        () ->
                                                                stores.ownerLeaseStore()
                                                                        .releaseOwnerLease(token),
                                                        deadlineNanos)
                                                .thenApply(ignored -> null));
    }

    @Override
    public void close() {
        StopState state =
                inStateLane(
                        () -> {
                            started = false;
                            ScheduledFuture<?> heartbeat = heartbeatTask;
                            heartbeatTask = null;
                            return new StopState(false, heartbeat, null, startupLifecycle, null);
                        });
        cancel(state.heartbeat());
        heartbeatExecutor.shutdownNow();
    }

    private void renewOwnerLeaseOnHeartbeat(CompletableFuture<Void> lifecycle) {
        long firedNanos = System.nanoTime();
        stateLane
                .<Supplier<CompletionStage<Boolean>>>runAsync(
                        () -> {
                            if (!started || !ownsStartupCore(lifecycle)) {
                                return () -> CompletableFuture.completedFuture(false);
                            }
                            heartbeatTask = null;
                            long expected = nextOwnerLeaseRenewalNanos;
                            Supplier<CompletionStage<Boolean>> renewal =
                                    ownerLeaseRenewalCore(lifecycle);
                            return () -> {
                                if (expected != 0L) {
                                    ZLinkRuntimeMetrics.record(
                                            "zlink.location.owner_lease.renew.lateness",
                                            Duration.ofNanos(Math.max(0L, firedNanos - expected)),
                                            Map.of());
                                }
                                return renewal.get();
                            };
                        })
                .thenCompose(Supplier::get)
                .whenComplete(
                        (ignored, failure) ->
                                stateLane.runAsync(
                                        () -> {
                                            if (ownsStartupCore(lifecycle)) {
                                                startHeartbeatCore();
                                            }
                                            return null;
                                        }));
    }

    private void recordSuccessfulRenewalCore(
            Instant leaseExpiresAt, Instant storeNow, long operationStartedNanos) {
        Duration admissionLifetime =
                Duration.between(storeNow, leaseExpiresAt).minus(ownerLeaseFencingMargin);
        if (admissionLifetime.isZero() || admissionLifetime.isNegative()) {
            throw new IllegalStateException(
                    "The owner lease does not leave a positive admission lifetime.");
        }
        ownerAdmissionDeadlineNanos =
                saturatingAdd(operationStartedNanos, admissionLifetime.toNanos());
        ownerLeaseRenewedAt = storeNow;
        lastError = null;
    }

    private CompletionStage<Void> recordFailure(String message) {
        return stateLane.runAsync(
                () -> {
                    recordFailureCore(message);
                    return null;
                });
    }

    private void recordFailureCore(String message) {
        lastError = message == null || message.isBlank() ? "owner lease operation failed" : message;
    }

    private static void cancel(ScheduledFuture<?> task) {
        if (task != null) {
            task.cancel(false);
        }
    }

    private record StartState(StartupLifecycle lifecycle, boolean claim) {}

    private record StopState(
            boolean shouldStop,
            ScheduledFuture<?> heartbeat,
            ZLinkLocationOwnerToken token,
            StartupLifecycle lifecycle,
            CompletableFuture<Void> completion) {}

    private record StartupLifecycle(
            CompletableFuture<Void> completion,
            CompletableFuture<Void> cleanup,
            Supplier<Instant> cleanupDeadline) {}

    private static final class OwnerLeaseClaimRejectedException extends IllegalStateException {
        OwnerLeaseClaimRejectedException(String message) {
            super(message);
        }
    }

    private record RenewState(
            RoutingId nodeRid, ZLinkLocationOwnerToken token, boolean admissionOpen) {}

    private static String failureMessage(Throwable failure) {
        Throwable current = unwrap(failure);
        String message = current.getMessage();
        return message == null || message.isBlank() ? current.getClass().getSimpleName() : message;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof CompletionException || current instanceof ExecutionException)
                && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank()) {
            throw new IllegalArgumentException(name + " must not be blank.");
        }
        return value;
    }

    private static Duration requirePositive(Duration value, String name) {
        if (value == null || value.isZero() || value.isNegative()) {
            throw new IllegalArgumentException(name + " must be positive.");
        }
        return value;
    }

    private static Duration requireNonNegative(Duration value, String name) {
        if (value == null || value.isNegative()) {
            throw new IllegalArgumentException(name + " must not be negative.");
        }
        return value;
    }

    private boolean isOwnerAdmissionOpenCore() {
        return ownerToken != null
                && ownerAdmissionDeadlineNanos != 0L
                && System.nanoTime() - ownerAdmissionDeadlineNanos < 0L;
    }

    private Optional<ZLinkLocationOwnerToken> admittedOwnerTokenCore() {
        return isOwnerAdmissionOpenCore() ? Optional.of(ownerToken) : Optional.empty();
    }

    private void ensureOwnerAdmissionOpenCore() {
        if (!isOwnerAdmissionOpenCore()) {
            throw new IllegalStateException("The owner lease admission deadline has expired.");
        }
    }

    private static long saturatingAdd(long left, long right) {
        long result = left + right;
        return ((left ^ result) & (right ^ result)) < 0L ? Long.MAX_VALUE : result;
    }

    private static Duration defaultOwnerLeaseRenewTimeout() {
        return new ZLinkLocationOptions().ownerLeaseRenewTimeout();
    }

    private static Duration defaultOwnerLeaseFencingMargin() {
        return new ZLinkLocationOptions().ownerLeaseFencingMargin();
    }
}
