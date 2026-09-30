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
import java.util.concurrent.atomic.AtomicBoolean;
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
    private final AtomicBoolean heartbeatInFlight = new AtomicBoolean();
    private ScheduledFuture<?> heartbeatTask;
    private CompletableFuture<Void> startupCompletion;
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
        Objects.requireNonNull(nodeRid, "nodeRid");
        StartState state =
                inStateLane(
                        () -> {
                            if (started) {
                                return new StartState(
                                        startupCompletion == null
                                                ? CompletableFuture.completedFuture(null)
                                                : startupCompletion,
                                        false);
                            }
                            started = true;
                            this.nodeRid = nodeRid;
                            startupCompletion = new CompletableFuture<>();
                            return new StartState(startupCompletion, true);
                        });
        if (state.claim()) {
            attemptInitialOwnerLeaseClaim(state.completion());
        }
        return state.completion();
    }

    public CompletionStage<Void> stop() {
        StopState state =
                inStateLane(
                        () -> {
                            boolean shouldStop = started;
                            started = false;
                            ScheduledFuture<?> heartbeat = heartbeatTask;
                            heartbeatTask = null;
                            return new StopState(
                                    shouldStop, heartbeat, admittedOwnerTokenCore().orElse(null));
                        });
        cancel(state.heartbeat());
        if (!state.shouldStop()) {
            return CompletableFuture.completedFuture(null);
        }

        ZLinkLocationOwnerToken token = state.token();
        CompletionStage<Long> cleanup =
                token == null
                        ? CompletableFuture.completedFuture(0L)
                        : stores.unifiedStore().removeAllByOwner(token);
        return cleanup.thenCompose(
                        ignored ->
                                token == null
                                        ? CompletableFuture.completedFuture(null)
                                        : stores.ownerLeaseStore()
                                                .releaseOwnerLease(token)
                                                .thenApply(released -> null))
                .thenCompose(
                        ignored ->
                                stateLane.runAsync(
                                        () -> {
                                            ownerToken = null;
                                            ownerAdmissionDeadlineNanos = 0L;
                                            return startupCompletion;
                                        }))
                .thenAccept(
                        completion -> {
                            if (completion != null && !completion.isDone()) {
                                completion.completeExceptionally(
                                        new IllegalStateException(
                                                "Location runtime stopped before owner lease became"
                                                        + " ready."));
                            }
                        });
    }

    public CompletionStage<Boolean> renewOwnerLeaseOnce() {
        return stateLane
                .runAsync(() -> new RenewState(nodeRid, ownerToken, isOwnerAdmissionOpenCore()))
                .thenCompose(this::renewOwnerLease);
    }

    private CompletionStage<Boolean> renewOwnerLease(RenewState state) {
        if (state.nodeRid() == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Location runtime must be started before renewing its owner lease."));
        }

        ZLinkLocationOwnerToken token = state.token();
        if (token == null) {
            return reportRenewalFailure(
                    claimOwnerLease()
                            .thenCompose(ignored -> republishAfterOwnerLeaseRecovery())
                            .thenApply(ignored -> true));
        }
        long operationStartedNanos = System.nanoTime();
        return reportRenewalFailure(
                stores.ownerLeaseStore()
                        .renewOwnerLease(token, ownerLeaseTtl)
                        .thenCompose(
                                result -> {
                                    if (result
                                            instanceof
                                            systems.zlink.framework.runtime.internal.locations
                                                            .ZLinkOwnerLeaseRenewed
                                                    renewed) {
                                        return stateLane
                                                .runAsync(
                                                        () -> {
                                                            recordSuccessfulRenewalCore(
                                                                    renewed.leaseExpiresAt(),
                                                                    renewed.storeNow(),
                                                                    operationStartedNanos);
                                                            nextOwnerLeaseRenewalNanos =
                                                                    System.nanoTime()
                                                                            + heartbeatInterval
                                                                                    .toNanos();
                                                            return null;
                                                        })
                                                .thenCompose(
                                                        ignored ->
                                                                state.admissionOpen()
                                                                        ? CompletableFuture
                                                                                .completedFuture(
                                                                                        true)
                                                                        : republishAfterOwnerLeaseRecovery()
                                                                                .thenApply(
                                                                                        ignoredValue ->
                                                                                                true));
                                    }

                                    // A lease can expire while the store is unavailable. The old
                                    // token cannot be renewed after recovery, so claim a fresh
                                    // generation before reporting the runtime as healthy again.
                                    return stateLane
                                            .runAsync(
                                                    () -> {
                                                        recordFailureCore(
                                                                "owner lease renewal was stale");
                                                        ownerAdmissionDeadlineNanos = 0L;
                                                        return null;
                                                    })
                                            .thenCompose(ignored -> claimOwnerLease())
                                            .thenCompose(
                                                    ignored ->
                                                            stateLane.runAsync(
                                                                    () -> {
                                                                        recoveryPreviousOwnerToken =
                                                                                token;
                                                                        return null;
                                                                    }))
                                            .thenCompose(
                                                    ignored -> republishAfterOwnerLeaseRecovery())
                                            .thenApply(ignored -> true);
                                }));
    }

    /** Completes a renewal as {@code false} after its failure is recorded on the state lane. */
    private CompletionStage<Boolean> reportRenewalFailure(CompletionStage<Boolean> renewal) {
        return renewal.exceptionallyCompose(
                failure -> recordFailure(failureMessage(failure)).thenApply(ignored -> false));
    }

    private CompletionStage<Void> claimOwnerLease() {
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        return resolveOwnerLeaseClaim(
                        deadlineNanos,
                        stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl))
                .thenCompose(claimed -> installOwnerLease(claimed, operationStartedNanos));
    }

    private CompletionStage<ZLinkOwnerLeaseClaimed> resolveOwnerLeaseClaim(
            long deadlineNanos, CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation) {
        return withinRenewDeadline(claimOperation, deadlineNanos)
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
            ZLinkOwnerLeaseClaimed claimed, long operationStartedNanos) {
        return stateLane.runAsync(
                () -> {
                    installOwnerLeaseCore(claimed, operationStartedNanos);
                    return null;
                });
    }

    private void installOwnerLeaseCore(ZLinkOwnerLeaseClaimed claimed, long operationStartedNanos) {
        ownerToken = claimed.token();
        recordSuccessfulRenewalCore(
                claimed.leaseExpiresAt(), claimed.storeNow(), operationStartedNanos);
        nextOwnerLeaseRenewalNanos = System.nanoTime() + heartbeatInterval.toNanos();
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterConflict(
            ZLinkOwnerLeaseClaimResult conflict, long deadlineNanos) {
        return withinRenewDeadline(
                        () -> stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
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
        return withinRenewDeadline(
                        () -> stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
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

    private <T> CompletionStage<T> withinRenewDeadline(
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

    private <T> CompletionStage<T> withinRenewDeadline(
            Supplier<CompletionStage<T>> operation, long deadlineNanos) {
        if (deadlineNanos - System.nanoTime() <= 0L) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("owner lease operation timed out"));
        }
        return withinRenewDeadline(operation.get(), deadlineNanos);
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery() {
        return stateLane
                .runAsync(() -> ownerLeaseRecoveryListener)
                .thenCompose(
                        listener ->
                                listener == null
                                        ? CompletableFuture.<Void>completedFuture(null)
                                        : listener.get()
                                                .exceptionallyCompose(
                                                        this::closeAdmissionAfterRepublishFailure));
    }

    private CompletionStage<Void> closeAdmissionAfterRepublishFailure(Throwable failure) {
        return stateLane
                .runAsync(
                        () -> {
                            ownerAdmissionDeadlineNanos = 0L;
                            return null;
                        })
                .thenCompose(ignored -> CompletableFuture.failedFuture(failure));
    }

    private void attemptInitialOwnerLeaseClaim(CompletableFuture<Void> completion) {
        boolean shouldClaim = inStateLane(() -> started && !completion.isDone());
        if (!shouldClaim) {
            return;
        }
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation =
                stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl);
        CompletableFuture<Void> cancellationCleanup = new CompletableFuture<>();
        completion.whenComplete(
                (ignored, failure) -> {
                    if (failure instanceof CancellationException) {
                        cancelStartupClaim(completion);
                        cancellationCleanup.whenComplete(
                                (cleanupResult, cleanupFailure) -> {
                                    if (cleanupFailure != null) {
                                        recordFailure(failureMessage(cleanupFailure));
                                    }
                                });
                    }
                });
        Runnable cleanup =
                () -> startStartupCancellationCleanup(cancellationCleanup, deadlineNanos);
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
                                                                completion,
                                                                cancelled,
                                                                failure,
                                                                cleanup)));
    }

    /**
     * Applies the startup claim result to lane state; returns whether the startup was abandoned.
     */
    private boolean installStartupClaimCore(
            CompletableFuture<Void> completion,
            ZLinkOwnerLeaseClaimed claimed,
            Throwable failure,
            long operationStartedNanos) {
        boolean ownsStartup = startupCompletion == completion;
        if (!ownsStartup || !started || completion.isCancelled()) {
            if (ownsStartup) {
                ownerToken = null;
                ownerAdmissionDeadlineNanos = 0L;
            }
            return true;
        }
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
            CompletableFuture<Void> completion,
            boolean cancelled,
            Throwable failure,
            Runnable cleanup) {
        if (cancelled) {
            cleanup.run();
            return CompletableFuture.completedFuture(null);
        }
        if (failure != null && unwrap(failure) instanceof OwnerLeaseClaimRejectedException) {
            if (!completion.completeExceptionally(unwrap(failure))) {
                cleanup.run();
            }
            return CompletableFuture.completedFuture(null);
        }
        return completeInitialClaim(completion)
                .thenAccept(
                        completed -> {
                            if (!completed) {
                                cleanup.run();
                            }
                        });
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
        if (heartbeatTask != null && !heartbeatTask.isCancelled()) {
            return;
        }
        heartbeatTask =
                heartbeatExecutor.scheduleWithFixedDelay(
                        this::renewOwnerLeaseOnHeartbeat,
                        heartbeatInterval.toMillis(),
                        heartbeatInterval.toMillis(),
                        TimeUnit.MILLISECONDS);
    }

    private void cancelStartupClaim(CompletableFuture<Void> completion) {
        ScheduledFuture<?> heartbeat =
                inStateLane(
                        () -> {
                            if (startupCompletion != completion) {
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

    private void startStartupCancellationCleanup(
            CompletableFuture<Void> cleanupCompletion, long deadlineNanos) {
        CompletionStage<Void> cleanup =
                withinRenewDeadline(
                                () -> stores.ownerLeaseStore().readOwnerLease(ownerId),
                                deadlineNanos)
                        .thenCompose(
                                result ->
                                        result
                                                                instanceof
                                                                systems.zlink.framework.runtime
                                                                                .internal.locations
                                                                                .ZLinkOwnerLeaseFound
                                                                        found
                                                        && ownerId.equals(found.token().ownerId())
                                                ? withinRenewDeadline(
                                                                () ->
                                                                        stores.ownerLeaseStore()
                                                                                .releaseOwnerLease(
                                                                                        found
                                                                                                .token()),
                                                                deadlineNanos)
                                                        .thenApply(ignored -> null)
                                                : CompletableFuture.completedFuture(null));
        cleanup.whenComplete(
                (ignored, failure) -> {
                    if (failure == null) {
                        cleanupCompletion.complete(null);
                    } else {
                        cleanupCompletion.completeExceptionally(unwrap(failure));
                    }
                });
    }

    @Override
    public void close() {
        StopState state =
                inStateLane(
                        () -> {
                            ScheduledFuture<?> heartbeat = heartbeatTask;
                            heartbeatTask = null;
                            return new StopState(false, heartbeat, null);
                        });
        cancel(state.heartbeat());
        heartbeatExecutor.shutdownNow();
    }

    private void renewOwnerLeaseOnHeartbeat() {
        if (!heartbeatInFlight.compareAndSet(false, true)) {
            return;
        }
        long firedNanos = System.nanoTime();
        stateLane
                .runAsync(() -> nextOwnerLeaseRenewalNanos)
                .thenCompose(
                        expected -> {
                            if (expected != 0L) {
                                ZLinkRuntimeMetrics.record(
                                        "zlink.location.owner_lease.renew.lateness",
                                        Duration.ofNanos(Math.max(0L, firedNanos - expected)),
                                        Map.of());
                            }
                            return renewOwnerLeaseOnce();
                        })
                .whenComplete((ignored, failure) -> heartbeatInFlight.set(false));
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
        Instant previous = ownerLeaseRenewedAt;
        ownerLeaseRenewedAt =
                previous == null || storeNow.isAfter(previous) ? storeNow : previous.plusNanos(1L);
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

    private record StartState(CompletableFuture<Void> completion, boolean claim) {}

    private record StopState(
            boolean shouldStop, ScheduledFuture<?> heartbeat, ZLinkLocationOwnerToken token) {}

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
