package systems.zlink.framework.runtime.locations;

import java.time.Duration;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;

public final class ZLinkLocationRuntime implements AutoCloseable {
    private final ZLinkRegisteredLocationStores stores;
    private final String ownerId;
    private final Duration ownerLeaseTtl;
    private final Duration heartbeatInterval;
    private final ScheduledExecutorService heartbeatExecutor;
    private final Object stateGate = new Object();
    private final java.util.concurrent.atomic.AtomicBoolean heartbeatInFlight =
        new java.util.concurrent.atomic.AtomicBoolean();
    private ScheduledFuture<?> heartbeatTask;
    private RoutingId nodeRid;
    private boolean started;
    private volatile boolean ownerLeaseHealthy;
    private volatile String lastError;
    private volatile java.time.Instant ownerLeaseRenewedAt;
    private volatile ZLinkLocationOwnerToken ownerToken;
    private volatile long nextOwnerLeaseRenewalNanos;

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
        ZLinkLocationOwnerToken current = ownerToken;
        if (current == null) {
            throw new IllegalStateException(
                "Location runtime owner lease is not ready.");
        }
        return current;
    }

    public ZLinkLocationOwnerToken currentOwnerToken() {
        return ownerTokenSnapshot();
    }

    ZLinkLocationRepository locationStore() {
        return stores.unifiedStore();
    }

    public boolean ownerLeaseHealthy() {
        return ownerLeaseHealthy;
    }

    public String lastError() {
        return lastError;
    }

    public java.time.Instant ownerLeaseRenewedAt() {
        return ownerLeaseRenewedAt;
    }

    public CompletionStage<Void> start(RoutingId nodeRid) {
        Objects.requireNonNull(nodeRid, "nodeRid");
        synchronized (stateGate) {
            if (started) {
                return CompletableFuture.completedFuture(null);
            }
            started = true;
            this.nodeRid = nodeRid;
        }

        return claimOwnerLease().thenAccept(ignored -> {
            synchronized (stateGate) {
                if (heartbeatTask == null || heartbeatTask.isCancelled()) {
                    heartbeatTask = heartbeatExecutor.scheduleWithFixedDelay(
                        this::renewOwnerLeaseOnHeartbeat,
                        heartbeatInterval.toMillis(),
                        heartbeatInterval.toMillis(),
                        TimeUnit.MILLISECONDS);
                }
            }
        });
    }

    public CompletionStage<Void> stop() {
        boolean shouldStop;
        synchronized (stateGate) {
            shouldStop = started;
            started = false;
            if (heartbeatTask != null) {
                heartbeatTask.cancel(false);
                heartbeatTask = null;
            }
        }
        if (!shouldStop) {
            return CompletableFuture.completedFuture(null);
        }

        ZLinkLocationOwnerToken token = ownerToken;
        CompletionStage<Long> cleanup = token == null
            ? CompletableFuture.completedFuture(0L)
            : stores.unifiedStore().removeAllByOwner(token);
        return cleanup
            .thenCompose(ignored -> token == null
                ? CompletableFuture.completedFuture(null)
                : stores.ownerLeaseStore().releaseOwnerLease(token)
                    .thenApply(released -> null))
            .thenRun(() -> ownerToken = null);
    }

    public CompletionStage<Boolean> renewOwnerLeaseOnce() {
        RoutingId currentNodeRid = nodeRid;
        if (currentNodeRid == null) {
            CompletableFuture<Boolean> failed = new CompletableFuture<>();
            failed.completeExceptionally(new IllegalStateException("Location runtime must be started before renewing its owner lease."));
            return failed;
        }

        ZLinkLocationOwnerToken token = ownerToken;
        if (token == null) {
            return CompletableFuture.completedFuture(false);
        }
        return stores.ownerLeaseStore().renewOwnerLease(token, ownerLeaseTtl)
            .thenCompose(result -> {
                if (result instanceof systems.zlink.framework.runtime.internal.locations
                    .ZLinkOwnerLeaseRenewed renewed) {
                    recordSuccessfulRenewal(renewed.storeNow());
                    nextOwnerLeaseRenewalNanos =
                        System.nanoTime() + heartbeatInterval.toNanos();
                    return CompletableFuture.completedFuture(true);
                }

                // A lease can expire while the store is unavailable. The old
                // token cannot be renewed after recovery, so claim a fresh
                // generation before reporting the runtime as healthy again.
                recordFailure("owner lease renewal was stale");
                return claimOwnerLease().handle((ignored, failure) -> {
                    if (failure != null) {
                        recordFailure(failureMessage(failure));
                        return false;
                    }
                    return true;
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
                if (result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed claimed) {
                    ownerToken = claimed.token();
                    recordSuccessfulRenewal(claimed.storeNow());
                    nextOwnerLeaseRenewalNanos =
                        System.nanoTime() + heartbeatInterval.toNanos();
                    return CompletableFuture.completedFuture(null);
                }
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        result instanceof systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted
                            ? "owner lease generation is exhausted"
                            : "owner lease is already claimed"));
            });
    }

    @Override
    public void close() {
        synchronized (stateGate) {
            if (heartbeatTask != null) {
                heartbeatTask.cancel(false);
                heartbeatTask = null;
            }
        }
        heartbeatExecutor.shutdownNow();
    }

    private void renewOwnerLeaseOnHeartbeat() {
        if (!heartbeatInFlight.compareAndSet(false, true)) {
            return;
        }
        long expected = nextOwnerLeaseRenewalNanos;
        if (expected != 0L) {
            long lateNanos = Math.max(0L, System.nanoTime() - expected);
            systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.record(
                "zlink.location.owner_lease.renew.lateness",
                java.time.Duration.ofNanos(lateNanos),
                java.util.Map.of());
        }
        renewOwnerLeaseOnce().whenComplete((ignored, failure) -> heartbeatInFlight.set(false));
    }

    private void recordSuccessfulRenewal(java.time.Instant storeNow) {
        synchronized (stateGate) {
            java.time.Instant previous = ownerLeaseRenewedAt;
            ownerLeaseRenewedAt = previous == null || storeNow.isAfter(previous)
                ? storeNow
                : previous.plusNanos(1L);
            ownerLeaseHealthy = true;
            lastError = null;
        }
    }

    private void recordFailure(String message) {
        ownerLeaseHealthy = false;
        lastError = message == null || message.isBlank()
            ? "owner lease operation failed"
            : message;
    }

    private static String failureMessage(Throwable failure) {
        Throwable current = failure;
        while ((current instanceof java.util.concurrent.CompletionException
            || current instanceof java.util.concurrent.ExecutionException)
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
