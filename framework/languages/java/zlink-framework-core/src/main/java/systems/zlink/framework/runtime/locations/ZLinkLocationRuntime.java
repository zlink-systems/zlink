package systems.zlink.framework.runtime.locations;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.atomic.AtomicBoolean;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Duration;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
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
    private final ScheduledExecutorService heartbeatExecutor;
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final AtomicBoolean heartbeatInFlight =
        new AtomicBoolean();
    private ScheduledTask heartbeatTask;
    private ScheduledTask initialClaimRetryTask;
    private CompletableFuture<Void> startupCompletion;
    private RoutingId nodeRid;
    private boolean started;
    private boolean ownerLeaseHealthy;
    private String lastError;
    private Instant ownerLeaseRenewedAt;
    private ZLinkLocationOwnerToken ownerToken;
    private ZLinkLocationOwnerToken recoveryPreviousOwnerToken;
    private boolean ownerLeaseRecoveryPending;
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
        this(ZLinkRegisteredLocationStores.fromUnified(store), UUID.randomUUID().toString().replace("-", ""), ownerLeaseTtl, heartbeatInterval);
    }

    ZLinkLocationRuntime(
        ZLinkLocationRepository store,
        String ownerId,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
        this(ZLinkRegisteredLocationStores.fromUnified(store), ownerId, ownerLeaseTtl, heartbeatInterval);
    }

    public ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
        this(stores, UUID.randomUUID().toString().replace("-", ""), ownerLeaseTtl, heartbeatInterval);
    }

    ZLinkLocationRuntime(
        ZLinkRegisteredLocationStores stores,
        String ownerId,
        Duration ownerLeaseTtl,
        Duration heartbeatInterval) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.ownerId = requireText(ownerId, "ownerId");
        this.ownerLeaseTtl = requirePositive(ownerLeaseTtl, "ownerLeaseTtl");
        this.heartbeatInterval = requirePositive(heartbeatInterval, "heartbeatInterval");
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
        ZLinkLocationOwnerToken current = inStateLane(() -> ownerToken);
        if (current == null) {
            throw new IllegalStateException(
                "Location runtime owner lease is not ready.");
        }
        return current;
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
        return inStateLane(() -> ownerLeaseHealthy);
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
            ScheduledTask initialRetry = initialClaimRetryTask;
            ScheduledTask heartbeat = heartbeatTask;
            initialClaimRetryTask = null;
            heartbeatTask = null;
            return new StopState(
                shouldStop,
                initialRetry,
                heartbeat,
                ownerToken);
        });
        cancel(state.initialRetry());
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
        RenewState state = inStateLane(() -> new RenewState(nodeRid, ownerToken));
        RoutingId currentNodeRid = state.nodeRid();
        if (currentNodeRid == null) {
            CompletableFuture<Boolean> failed = new CompletableFuture<>();
            failed.completeExceptionally(new IllegalStateException("Location runtime must be started before renewing its owner lease."));
            return failed;
        }

        ZLinkLocationOwnerToken token = state.token();
        if (token == null) {
            return CompletableFuture.completedFuture(false);
        }
        return stores.ownerLeaseStore().renewOwnerLease(token, ownerLeaseTtl)
            .thenCompose(result -> {
                if (result instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkOwnerLeaseRenewed renewed) {
                    inStateLane(() -> {
                        recordSuccessfulRenewalCore(renewed.storeNow());
                        nextOwnerLeaseRenewalNanos =
                            System.nanoTime() + heartbeatInterval.toNanos();
                        return null;
                    });
                    return republishAfterOwnerLeaseRecovery()
                        .thenApply(ignored -> true);
                }

                // A lease can expire while the store is unavailable. The old
                // token cannot be renewed after recovery, so claim a fresh
                // generation before reporting the runtime as healthy again.
                recordFailure("owner lease renewal was stale");
                return claimOwnerLease().thenCompose(ignored -> {
                    inStateLane(() -> {
                        recoveryPreviousOwnerToken = token;
                        ownerLeaseRecoveryPending = true;
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
        return stores.ownerLeaseStore()
            .claimOwnerLease(ownerId, ownerLeaseTtl)
            .thenCompose(result -> {
                if (result instanceof ZLinkOwnerLeaseClaimed claimed) {
                    inStateLane(() -> {
                        ownerToken = claimed.token();
                        recordSuccessfulRenewalCore(claimed.storeNow());
                        nextOwnerLeaseRenewalNanos =
                            System.nanoTime() + heartbeatInterval.toNanos();
                        return null;
                    });
                    return CompletableFuture.completedFuture(null);
                }
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        result instanceof ZLinkOwnerLeaseGenerationExhausted
                            ? "owner lease generation is exhausted"
                            : "owner lease is already claimed"));
            });
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery() {
        Supplier<CompletionStage<Void>> listener = inStateLane(() -> {
            if (!ownerLeaseRecoveryPending) {
                return null;
            }
            if (ownerLeaseRecoveryListener == null) {
                ownerLeaseRecoveryPending = false;
                return null;
            }
            return ownerLeaseRecoveryListener;
        });
        if (listener == null) {
            return CompletableFuture.completedFuture(null);
        }
        try {
            return listener.get().thenRun(
                () -> inStateLane(() -> {
                    ownerLeaseRecoveryPending = false;
                    return null;
                }));
        } catch (RuntimeException recoveryFailure) {
            return CompletableFuture.failedFuture(recoveryFailure);
        }
    }

    private void attemptInitialOwnerLeaseClaim(
        CompletableFuture<Void> completion) {
        boolean shouldClaim = inStateLane(() -> started && !completion.isDone());
        if (!shouldClaim) {
            return;
        }
        claimOwnerLease().whenComplete((ignored, failure) -> {
            if (failure == null) {
                ScheduledTask heartbeat = inStateLane(() -> {
                    if (heartbeatTask != null && !heartbeatTask.isCancelled()) {
                        return null;
                    }
                    heartbeatTask = new ScheduledTask();
                    return heartbeatTask;
                });
                if (heartbeat != null) {
                    ScheduledFuture<?> future = heartbeatExecutor.scheduleWithFixedDelay(
                        this::renewOwnerLeaseOnHeartbeat,
                        heartbeatInterval.toMillis(),
                        heartbeatInterval.toMillis(),
                        TimeUnit.MILLISECONDS);
                    boolean cancel = inStateLane(() -> {
                        if (heartbeatTask == heartbeat) {
                            heartbeat.attach(future);
                            return false;
                        }
                        return true;
                    });
                    if (cancel) {
                        future.cancel(false);
                    }
                }
                completion.complete(null);
                return;
            }
            recordFailure(failureMessage(failure));
            ScheduledTask retry = inStateLane(() -> {
                if (!started || completion.isDone()) {
                    return null;
                }
                initialClaimRetryTask = new ScheduledTask();
                return initialClaimRetryTask;
            });
            if (retry != null) {
                ScheduledFuture<?> future = heartbeatExecutor.schedule(
                    () -> {
                        boolean retryCurrent = inStateLane(() -> {
                            if (initialClaimRetryTask != retry) {
                                return false;
                            }
                            initialClaimRetryTask = null;
                            return true;
                        });
                        if (retryCurrent) {
                            attemptInitialOwnerLeaseClaim(completion);
                        }
                    },
                    heartbeatInterval.toMillis(),
                    TimeUnit.MILLISECONDS);
                boolean cancel = inStateLane(() -> {
                    if (initialClaimRetryTask != retry) {
                        return true;
                    }
                    retry.attach(future);
                    return false;
                });
                if (cancel) {
                    future.cancel(false);
                }
            }
        });
    }

    @Override
    public void close() {
        StopState state = inStateLane(() -> {
            ScheduledTask initialRetry = initialClaimRetryTask;
            ScheduledTask heartbeat = heartbeatTask;
            initialClaimRetryTask = null;
            heartbeatTask = null;
            return new StopState(false, initialRetry, heartbeat, null);
        });
        cancel(state.initialRetry());
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

    private void recordSuccessfulRenewalCore(Instant storeNow) {
        Instant previous = ownerLeaseRenewedAt;
        ownerLeaseRenewedAt = previous == null || storeNow.isAfter(previous)
            ? storeNow
            : previous.plusNanos(1L);
        ownerLeaseHealthy = true;
        lastError = null;
    }

    private void recordFailure(String message) {
        inStateLane(() -> {
            ownerLeaseHealthy = false;
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
        ScheduledTask initialRetry,
        ScheduledTask heartbeat,
        ZLinkLocationOwnerToken token) {}

    private record RenewState(
        RoutingId nodeRid,
        ZLinkLocationOwnerToken token) {}

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
        Throwable current = failure;
        while ((current instanceof CompletionException
            || current instanceof ExecutionException)
            && current.getCause() != null) {
            current = current.getCause();
        }
        String message = current.getMessage();
        return message == null || message.isBlank()
            ? current.getClass().getSimpleName()
            : message;
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
}
