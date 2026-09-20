package systems.zlink.framework.runtime.locations;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CancellationException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.locations.ZLinkLocationOptions;

import java.time.Duration;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;

public final class ZLinkLocationRuntime implements AutoCloseable {
    private final ZLinkRegisteredLocationStores stores;
    private final String ownerId;
    private final Duration ownerLeaseTtl;
    private final Duration heartbeatInterval;
    private final Duration ownerLeaseRenewTimeout;
    private final Duration ownerLeaseFencingMargin;
    private final ScheduledExecutorService heartbeatExecutor;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final AtomicBoolean heartbeatInFlight =
        new AtomicBoolean();
    private ScheduledTask heartbeatTask;
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
        ZLinkLocationRepository store,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
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
            ZLinkRegisteredLocationStores.fromUnified(store), ownerId,
            ownerLeaseTtl, heartbeatInterval,
            defaultOwnerLeaseRenewTimeout(),
            defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
        this(
            stores, UUID.randomUUID().toString().replace("-", ""),
            ownerLeaseTtl, heartbeatInterval,
            defaultOwnerLeaseRenewTimeout(),
            defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval,
        Duration ownerLeaseRenewTimeout) {
        this(
            stores, UUID.randomUUID().toString().replace("-", ""),
            ownerLeaseTtl, heartbeatInterval, ownerLeaseRenewTimeout,
            defaultOwnerLeaseFencingMargin());
    }

    public ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval,
        Duration ownerLeaseRenewTimeout,
        Duration ownerLeaseFencingMargin) {
        this(
            stores, UUID.randomUUID().toString().replace("-", ""),
            ownerLeaseTtl, heartbeatInterval, ownerLeaseRenewTimeout,
            ownerLeaseFencingMargin);
    }

    ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        String ownerId,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
        this(
            stores, ownerId, ownerLeaseTtl, heartbeatInterval,
            defaultOwnerLeaseRenewTimeout(),
            defaultOwnerLeaseFencingMargin());
    }

    ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        String ownerId,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval,
        Duration ownerLeaseRenewTimeout) {
        this(stores, ownerId, ownerLeaseTtl, heartbeatInterval,
            ownerLeaseRenewTimeout, defaultOwnerLeaseFencingMargin());
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
        this.ownerLeaseRenewTimeout = requirePositive(
            ownerLeaseRenewTimeout, "ownerLeaseRenewTimeout");
        this.ownerLeaseFencingMargin = requireNonNegative(
            ownerLeaseFencingMargin, "ownerLeaseFencingMargin");
        this.heartbeatExecutor = Executors.newSingleThreadScheduledExecutor(task -> {
            Thread thread = new Thread(task, "zlink-location-owner-lease");
            thread.setDaemon(true);
            return thread;
        });
    }

    public String ownerId() {
        return ownerId;
    }

    ZLinkLocationOwnerToken ownerTokenSnapshot() {
        return inStateLane(() -> {
            ensureOwnerAdmissionOpenCore();
            return ownerToken;
        });
    }

    public ZLinkLocationOwnerToken currentOwnerToken() {
        return ownerTokenSnapshot();
    }

    public ZLinkLocationOwnerToken recoveryPreviousOwnerToken() {
        return inStateLane(() -> recoveryPreviousOwnerToken);
    }

    /**
     * Registers the runtime action that republishes owner-scoped records after
     * a stale lease is replaced by a new generation.
     */
    public void setOwnerLeaseRecoveryListener(
        Supplier<CompletionStage<Void>> listener) {
        inStateLane(() -> {
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
        inStateLane(() -> {
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
        StartState state = inStateLane(() -> {
            if (started) {
                return new StartState(startupCompletion == null
                    ? CompletableFuture.completedFuture(null)
                    : startupCompletion, false);
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
        StopState state = inStateLane(() -> {
            boolean shouldStop = started;
            started = false;
            ScheduledTask heartbeat = heartbeatTask;
            heartbeatTask = null;
            return new StopState(
                shouldStop,
                heartbeat,
                ownerToken);
        });
        cancel(state.heartbeat());
        if (!state.shouldStop()) {
            return CompletableFuture.completedFuture(null);
        }

        ZLinkLocationOwnerToken token = state.token();
        CompletionStage<Long> cleanup = token == null
            ? CompletableFuture.completedFuture(0L)
            : stores.unifiedStore().removeAllByOwner(token);
        return cleanup
            .thenCompose(ignored -> token == null
                ? CompletableFuture.completedFuture(null)
                : stores.ownerLeaseStore().releaseOwnerLease(token)
                    .thenApply(released -> null))
            .thenRun(() -> {
                CompletableFuture<Void> completion = inStateLane(() -> {
                    ownerToken = null;
                    ownerAdmissionDeadlineNanos = 0L;
                    return startupCompletion;
                });
                if (completion != null && !completion.isDone()) {
                    completion.completeExceptionally(
                        new IllegalStateException(
                            "Location runtime stopped before owner lease became ready."));
                }
            });
    }

    public CompletionStage<Boolean> renewOwnerLeaseOnce() {
        RenewState state = inStateLane(() -> new RenewState(
            nodeRid, ownerToken, isOwnerAdmissionOpenCore()));
        RoutingId currentNodeRid = state.nodeRid();
        if (currentNodeRid == null) {
            CompletableFuture<Boolean> failed = new CompletableFuture<>();
            failed.completeExceptionally(new IllegalStateException("Location runtime must be started before renewing its owner lease."));
            return failed;
        }

        ZLinkLocationOwnerToken token = state.token();
        if (token == null) {
            return claimOwnerLease()
                .thenCompose(ignored -> republishAfterOwnerLeaseRecovery())
                .thenApply(ignored -> true)
                .handle((result, failure) -> {
                    if (failure != null) {
                        recordFailure(failureMessage(failure));
                        return false;
                    }
                    return result;
                });
        }
        long operationStartedNanos = System.nanoTime();
        return stores.ownerLeaseStore().renewOwnerLease(token, ownerLeaseTtl)
            .thenCompose(result -> {
                if (result instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkOwnerLeaseRenewed renewed) {
                    inStateLane(() -> {
                        recordSuccessfulRenewalCore(
                            renewed.leaseExpiresAt(), renewed.storeNow(),
                            operationStartedNanos);
                        nextOwnerLeaseRenewalNanos =
                            System.nanoTime() + heartbeatInterval.toNanos();
                        return null;
                    });
                    return state.admissionOpen()
                        ? CompletableFuture.completedFuture(true)
                        : republishAfterOwnerLeaseRecovery()
                            .thenApply(ignored -> true);
                }

                // A lease can expire while the store is unavailable. The old
                // token cannot be renewed after recovery, so claim a fresh
                // generation before reporting the runtime as healthy again.
                recordFailure("owner lease renewal was stale");
                inStateLane(() -> {
                    ownerAdmissionDeadlineNanos = 0L;
                    return null;
                });
                return claimOwnerLease().thenCompose(ignored -> {
                    inStateLane(() -> {
                        recoveryPreviousOwnerToken = token;
                        return null;
                    });
                    return republishAfterOwnerLeaseRecovery().thenApply(
                        ignoredValue -> true);
                });
            })
            .handle((result, failure) -> {
                if (failure != null) {
                    recordFailure(failureMessage(failure));
                    return false;
                }
                return result;
            });
    }

    private CompletionStage<Void> claimOwnerLease() {
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(
            operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        return claimOwnerLease(
            operationStartedNanos,
            deadlineNanos,
            stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl));
    }

    private CompletionStage<Void> claimOwnerLease(
        long operationStartedNanos,
        long deadlineNanos,
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation) {
        return withinRenewDeadline(
                claimOperation, deadlineNanos)
            .<CompletionStage<ZLinkOwnerLeaseClaimResult>>handle((result, failure) -> {
                if (failure == null) {
                    return CompletableFuture.completedFuture(result);
                }
                return confirmClaimAfterFailure(failure, deadlineNanos);
            })
            .thenCompose(result -> result)
            .thenCompose(result -> result instanceof systems.zlink.framework.runtime
                .internal.locations.ZLinkOwnerLeaseClaimConflict
                    ? confirmClaimAfterConflict(result, deadlineNanos)
                    : CompletableFuture.completedFuture(result))
            .thenCompose(result -> {
                if (result instanceof ZLinkOwnerLeaseClaimed claimed) {
                    inStateLane(() -> {
                        ownerToken = claimed.token();
                        recordSuccessfulRenewalCore(
                            claimed.leaseExpiresAt(), claimed.storeNow(),
                            operationStartedNanos);
                        nextOwnerLeaseRenewalNanos =
                            System.nanoTime() + heartbeatInterval.toNanos();
                        return null;
                    });
                    return CompletableFuture.completedFuture(null);
                }
                return CompletableFuture.failedFuture(
                    new OwnerLeaseClaimRejectedException(
                        result instanceof ZLinkOwnerLeaseGenerationExhausted
                            ? "owner lease generation is exhausted"
                            : "owner lease is already claimed"));
            });
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterConflict(
        ZLinkOwnerLeaseClaimResult conflict,
        long deadlineNanos) {
        return withinRenewDeadline(
                stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
            .handle((result, failure) -> failure == null
                    && result instanceof systems.zlink.framework.runtime
                        .internal.locations.ZLinkOwnerLeaseFound found
                    && ownerId.equals(found.token().ownerId())
                ? new ZLinkOwnerLeaseClaimed(
                    found.token(), found.leaseExpiresAt(), found.storeNow())
                : conflict);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterFailure(
        Throwable failure,
        long deadlineNanos) {
        return withinRenewDeadline(
                stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
            .thenCompose(result -> result instanceof systems.zlink.framework.runtime
                .internal.locations.ZLinkOwnerLeaseFound found
                    && ownerId.equals(found.token().ownerId())
                ? CompletableFuture.completedFuture(
                    new ZLinkOwnerLeaseClaimed(
                        found.token(), found.leaseExpiresAt(), found.storeNow()))
                : CompletableFuture.failedFuture(failure));
    }

    private <T> CompletionStage<T> withinRenewDeadline(
        CompletionStage<T> operation,
        long deadlineNanos) {
        long remainingNanos = deadlineNanos - System.nanoTime();
        if (remainingNanos <= 0L) {
            return CompletableFuture.failedFuture(new TimeoutException(
                "owner lease operation timed out"));
        }
        CompletableFuture<T> completion = new CompletableFuture<>();
        ScheduledFuture<?> timeout = heartbeatExecutor.schedule(
            () -> completion.completeExceptionally(new TimeoutException(
                "owner lease operation timed out")),
            remainingNanos,
            TimeUnit.NANOSECONDS);
        operation.whenComplete((result, failure) -> {
            if (failure == null && completion.complete(result)) {
                timeout.cancel(false);
            } else if (failure != null) {
                completion.completeExceptionally(failure);
                timeout.cancel(false);
            }
        });
        return completion;
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery() {
        Supplier<CompletionStage<Void>> listener = inStateLane(
            () -> ownerLeaseRecoveryListener);
        if (listener == null) {
            return CompletableFuture.completedFuture(null);
        }
        return listener.get().whenComplete((ignored, failure) -> {
            if (failure != null) {
                inStateLane(() -> {
                    ownerAdmissionDeadlineNanos = 0L;
                    return null;
                });
            }
        });
    }

    private void attemptInitialOwnerLeaseClaim(
        CompletableFuture<Void> completion) {
        boolean shouldClaim = inStateLane(() -> started && !completion.isDone());
        if (!shouldClaim) {
            return;
        }
        long operationStartedNanos = System.nanoTime();
        long deadlineNanos = saturatingAdd(
            operationStartedNanos, ownerLeaseRenewTimeout.toNanos());
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation =
            stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl);
        completion.whenComplete((ignored, failure) -> {
            if (failure instanceof CancellationException) {
                cancelStartupClaim(claimOperation, deadlineNanos);
            }
        });
        claimOwnerLease(operationStartedNanos, deadlineNanos, claimOperation)
            .whenComplete((ignored, failure) -> {
            if (failure == null) {
                completeInitialClaim(completion);
                return;
            }
            recordFailure(failureMessage(failure));
            if (unwrap(failure) instanceof OwnerLeaseClaimRejectedException) {
                inStateLane(() -> {
                    started = false;
                    return null;
                });
                completion.completeExceptionally(unwrap(failure));
                return;
            }
            completeInitialClaim(completion);
        });
    }

    private void completeInitialClaim(CompletableFuture<Void> completion) {
        boolean running = inStateLane(() -> started && !completion.isCancelled());
        if (!running) {
            return;
        }
        startHeartbeat();
        completion.complete(null);
    }

    private void startHeartbeat() {
        ScheduledTask heartbeat = inStateLane(() -> {
            if (!started || (heartbeatTask != null && !heartbeatTask.isCancelled())) {
                return null;
            }
            heartbeatTask = new ScheduledTask();
            return heartbeatTask;
        });
        if (heartbeat == null) {
            return;
        }
        ScheduledFuture<?> future = heartbeatExecutor.scheduleWithFixedDelay(
            this::renewOwnerLeaseOnHeartbeat,
            heartbeatInterval.toMillis(), heartbeatInterval.toMillis(),
            TimeUnit.MILLISECONDS);
        boolean cancel = inStateLane(() -> {
            if (heartbeatTask == heartbeat && started) {
                heartbeat.attach(future);
                return false;
            }
            return true;
        });
        if (cancel) {
            future.cancel(false);
        }
    }

    private void cancelStartupClaim(
        CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation,
        long deadlineNanos) {
        ScheduledTask heartbeat = inStateLane(() -> {
            started = false;
            ScheduledTask current = heartbeatTask;
            heartbeatTask = null;
            return current;
        });
        cancel(heartbeat);
        withinRenewDeadline(claimOperation, deadlineNanos)
            .whenComplete((ignored, failure) ->
                releaseClaimedLeaseAfterStartupCancellation(deadlineNanos));
    }

    private void releaseClaimedLeaseAfterStartupCancellation(long deadlineNanos) {
        withinRenewDeadline(
                stores.ownerLeaseStore().readOwnerLease(ownerId), deadlineNanos)
            .thenCompose(result -> result instanceof systems.zlink.framework.runtime
                    .internal.locations.ZLinkOwnerLeaseFound found
                    && ownerId.equals(found.token().ownerId())
                ? withinRenewDeadline(
                    stores.ownerLeaseStore().releaseOwnerLease(found.token()),
                    deadlineNanos).thenApply(ignored -> null)
                : CompletableFuture.completedFuture(null))
            .whenComplete((ignored, failure) -> {
                if (failure != null) {
                    recordFailure(failureMessage(failure));
                }
            });
    }

    @Override
    public void close() {
        StopState state = inStateLane(() -> {
            ScheduledTask heartbeat = heartbeatTask;
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
        long expected = inStateLane(() -> nextOwnerLeaseRenewalNanos);
        if (expected != 0L) {
            long lateNanos = Math.max(0L, System.nanoTime() - expected);
            ZLinkRuntimeMetrics.record(
                "zlink.location.owner_lease.renew.lateness",
                Duration.ofNanos(lateNanos),
                Map.of());
        }
        renewOwnerLeaseOnce().whenComplete((ignored, failure) -> heartbeatInFlight.set(false));
    }

    private void recordSuccessfulRenewalCore(
        Instant leaseExpiresAt,
        Instant storeNow,
        long operationStartedNanos) {
        Duration admissionLifetime = Duration.between(
            storeNow, leaseExpiresAt).minus(ownerLeaseFencingMargin);
        if (admissionLifetime.isZero() || admissionLifetime.isNegative()) {
            throw new IllegalStateException(
                "The owner lease does not leave a positive admission lifetime.");
        }
        ownerAdmissionDeadlineNanos = saturatingAdd(
            operationStartedNanos, admissionLifetime.toNanos());
        Instant previous = ownerLeaseRenewedAt;
        ownerLeaseRenewedAt = previous == null || storeNow.isAfter(previous)
            ? storeNow
            : previous.plusNanos(1L);
        lastError = null;
    }

    private void recordFailure(String message) {
        inStateLane(() -> {
            lastError = message == null || message.isBlank()
                ? "owner lease operation failed"
                : message;
            return null;
        });
    }

    private static void cancel(ScheduledTask task) {
        if (task != null) {
            task.cancel();
        }
    }

    private record StartState(CompletableFuture<Void> completion, boolean claim) {}

    private record StopState(
        boolean shouldStop,
        ScheduledTask heartbeat,
        ZLinkLocationOwnerToken token) {}

    private static final class OwnerLeaseClaimRejectedException
        extends IllegalStateException {
        OwnerLeaseClaimRejectedException(String message) {
            super(message);
        }
    }

    private record RenewState(
        RoutingId nodeRid,
        ZLinkLocationOwnerToken token,
        boolean admissionOpen) {}

    private static final class ScheduledTask {
        private ScheduledFuture<?> future;

        boolean isCancelled() {
            return future != null && future.isCancelled();
        }

        void attach(ScheduledFuture<?> value) {
            future = value;
        }

        void cancel() {
            if (future != null) {
                future.cancel(false);
            }
        }
    }

    private static String failureMessage(Throwable failure) {
        Throwable current = unwrap(failure);
        String message = current.getMessage();
        return message == null || message.isBlank()
            ? current.getClass().getSimpleName()
            : message;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof CompletionException
            || current instanceof ExecutionException)
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

    private void ensureOwnerAdmissionOpenCore() {
        if (!isOwnerAdmissionOpenCore()) {
            throw new IllegalStateException(
                "The owner lease admission deadline has expired.");
        }
    }

    private static long saturatingAdd(long left, long right) {
        long result = left + right;
        return ((left ^ result) & (right ^ result)) < 0L
            ? Long.MAX_VALUE
            : result;
    }

    private static Duration defaultOwnerLeaseRenewTimeout() {
        return new ZLinkLocationOptions().ownerLeaseRenewTimeout();
    }

    private static Duration defaultOwnerLeaseFencingMargin() {
        return new ZLinkLocationOptions().ownerLeaseFencingMargin();
    }
}
