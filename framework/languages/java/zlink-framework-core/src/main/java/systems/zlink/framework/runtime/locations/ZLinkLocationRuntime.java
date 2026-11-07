package systems.zlink.framework.runtime.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.execution.ZLinkExecutionAvailability;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationStoreFailure;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectServerDescriptorPublisher;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseGenerationExhausted;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Function;
import java.util.function.LongSupplier;
import java.util.function.Supplier;

public final class ZLinkLocationRuntime implements AutoCloseable {
    static final LongSupplier SYSTEM_MONOTONIC_NANOS = System::nanoTime;
    private final ZLinkRegisteredLocationStores stores;
    private final String ownerId;
    private List<Map<String, String>> metricScopes = List.of();

    public void metricScopes(List<Map<String, String>> scopes) {
        metricScopes = List.copyOf(scopes);
    }

    private void recordLeaseLateness(Duration lateness) {
        for (var tags : metricScopes)
            ZLinkRuntimeMetrics.record(
                    ZLinkRuntimeMetrics.Metric.LOCATION_OWNER_LEASE_RENEW_LATENESS.metricName(),
                    lateness,
                    tags);
    }

    private void recordLeaseFailure() {
        for (var tags : metricScopes)
            ZLinkRuntimeMetrics.increment(
                    ZLinkRuntimeMetrics.Metric.LOCATION_OWNER_LEASE_RENEW_FAILURES.metricName(),
                    tags);
    }

    private final Duration ownerLeaseTtl;
    private final Duration heartbeatInterval;
    private final Duration ownerLeaseRenewTimeout;
    private final Duration ownerLeaseFencingMargin;
    final ScheduledExecutorService heartbeatExecutor;
    final LongSupplier monotonicNow;
    final ZLinkStateLane stateLane;
    private ScheduledFuture<?> heartbeatTask;
    private boolean storeOutageRetention;
    private Long lastSuccessfulLeaseRequestStartedNanos;
    private volatile StoreOutage outage;
    // Lane-published fast path; deadlines remain owned by the lease/outage policy below.
    private volatile boolean executionOpen;
    private final Map<Object, Execution> executions = new HashMap<>();
    private ScheduledFuture<?> outageDeadlineTask;
    private StartupLifecycle startupLifecycle;
    private RoutingId nodeRid;
    private volatile boolean started;
    private volatile long ownerAdmissionDeadlineNanos;
    // Revocation used to erase the deadline with a zero sentinel. Keep the time for outage capture.
    private volatile boolean ownerAdmissionRevoked = true;
    private String lastError;
    private Instant ownerLeaseRenewedAt;
    private volatile ZLinkLocationOwnerToken ownerToken;
    private ZLinkLocationOwnerToken recoveryPreviousOwnerToken;
    private long ownerLeaseOperationStartedNanos;
    private Function<Boolean, CompletionStage<Void>> ownerLeaseRecoveryListener;
    private CompletableFuture<Void> ownerLeaseRecoveryOperation;

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
        this(
                stores,
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                ownerLeaseFencingMargin,
                SYSTEM_MONOTONIC_NANOS);
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin,
            LongSupplier monotonicNow) {
        this(
                stores,
                UUID.randomUUID().toString().replace("-", ""),
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                ownerLeaseFencingMargin,
                monotonicNow);
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin,
            LongSupplier monotonicNow) {
        this(
                stores,
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                ownerLeaseFencingMargin,
                monotonicNow,
                newHeartbeatExecutor());
    }

    private static ScheduledExecutorService newHeartbeatExecutor() {
        return Executors.newSingleThreadScheduledExecutor(
                task -> {
                    Thread thread = new Thread(task, "zlink-location-owner-lease");
                    thread.setDaemon(true);
                    return thread;
                });
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin,
            LongSupplier monotonicNow,
            ScheduledExecutorService heartbeatExecutor) {
        this(
                stores,
                ownerId,
                ownerLeaseTtl,
                heartbeatInterval,
                ownerLeaseRenewTimeout,
                ownerLeaseFencingMargin,
                monotonicNow,
                heartbeatExecutor,
                new ZLinkStateLane());
    }

    ZLinkLocationRuntime(
            ZLinkRegisteredLocationStores stores,
            String ownerId,
            Duration ownerLeaseTtl,
            Duration heartbeatInterval,
            Duration ownerLeaseRenewTimeout,
            Duration ownerLeaseFencingMargin,
            LongSupplier monotonicNow,
            ScheduledExecutorService heartbeatExecutor,
            ZLinkStateLane stateLane) {
        this.stores = Objects.requireNonNull(stores, "stores");
        this.ownerId = requireText(ownerId, "ownerId");
        this.ownerLeaseTtl = requirePositive(ownerLeaseTtl, "ownerLeaseTtl");
        this.heartbeatInterval = requirePositive(heartbeatInterval, "heartbeatInterval");
        this.ownerLeaseRenewTimeout =
                requirePositive(ownerLeaseRenewTimeout, "ownerLeaseRenewTimeout");
        this.ownerLeaseFencingMargin =
                requireNonNegative(ownerLeaseFencingMargin, "ownerLeaseFencingMargin");
        this.monotonicNow = Objects.requireNonNull(monotonicNow, "monotonicNow");
        this.heartbeatExecutor = Objects.requireNonNull(heartbeatExecutor, "heartbeatExecutor");
        this.stateLane = Objects.requireNonNull(stateLane, "stateLane");
    }

    /** Captures the startup option; runtime policy cannot be changed after startup. */
    public void setStoreOutageRetention(boolean value) {
        inStateLane(
                () -> {
                    if (startupLifecycle != null) {
                        throw new IllegalStateException("StoreOutageRetention is startup-only.");
                    }
                    storeOutageRetention = value;
                    return null;
                });
    }

    /** Lease and outage snapshot for diagnostics; execution consumes the same episode. */
    public OwnerLeaseSnapshot ownerLeaseSnapshot() {
        return inStateLane(
                () ->
                        new OwnerLeaseSnapshot(
                                ownerToken,
                                lastSuccessfulLeaseRequestStartedNanos,
                                outage == null ? null : outage.observedAtNanos,
                                outage == null ? null : outage.cutoffNanos,
                                outage != null && outage.discarded));
    }

    public record OwnerLeaseSnapshot(
            ZLinkLocationOwnerToken token,
            Long lastSuccessfulRequestStartedNanos,
            Long outageObservedAtNanos,
            Long outageCutoffNanos,
            boolean outageDiscarded) {}

    /** Incarnation execution policy; only this owner interprets the outage and lease deadlines. */
    public final class Execution
            implements AutoCloseable,
                    systems.zlink.framework.execution.ZLinkSerialExecutionQueue.ExecutionPolicy,
                    ZLinkExecutionAvailability {
        private final BooleanSupplier steady;
        private final AtomicReference<Runnable> cleanup;
        private final Supplier<String> authorityKey;
        private volatile String key;
        private final Function<ZLinkAuthoritySnapshot, CompletionStage<Void>> apply;
        private final Supplier<String> parent;
        private ZLinkAuthoritySnapshot confirmed;
        private volatile Runnable resume;

        private Execution(
                Supplier<String> authorityKey,
                BooleanSupplier steady,
                Function<ZLinkAuthoritySnapshot, CompletionStage<Void>> apply,
                Supplier<String> parent,
                Runnable cleanup) {
            this.authorityKey = authorityKey;
            this.steady = steady;
            this.apply = apply;
            this.parent = parent;
            this.cleanup = new AtomicReference<>(cleanup);
        }

        public CompletionStage<Void> ready() {
            String registeredKey = authorityKey.get();
            return stateLane
                    .runAsync(
                            () -> {
                                if (cleanup.get() != null) {
                                    executions.remove(key == null ? this : key, this);
                                    key = registeredKey;
                                    executions.put(key == null ? this : key, this);
                                }
                                return null;
                            })
                    .thenCompose(
                            ignored ->
                                    registeredKey == null
                                            ? CompletableFuture
                                                    .<ZLinkAuthorityReadResult>completedFuture(null)
                                            : locationStore().read(registeredKey, () -> false))
                    .thenCompose(
                            read ->
                                    stateLane.runAsync(
                                            () -> {
                                                if (cleanup.get() != null
                                                        && read
                                                                instanceof
                                                                ZLinkAuthoritySnapshot snapshot)
                                                    confirmed = snapshot;
                                                return null;
                                            }));
        }

        @Override
        public boolean paused() {
            StoreOutage episode = outage;
            return cleanup.get() != null
                    && episode != null
                    && episode.records.containsKey(this)
                    && (episode.publication != null || !Objects.equals(episode.token, ownerToken));
        }

        @Override
        public void onResume(Runnable action) {
            resume = action;
        }

        private void resumeCore() {
            Runnable action = resume;
            if (action != null) action.run();
        }

        @Override
        public void check(boolean application) {
            StoreOutage episode = outage;
            boolean retained = episode != null && episode.records.containsKey(this);
            if (executionOpen && !retained && cleanup.get() != null) return;
            if (!started)
                throw systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin.framework(
                        ZLinkFrameworkErrorKind.SHUTTING_DOWN, "Location runtime is stopped.");
            if (cleanup.get() == null
                    || (retained && (episode.discarded || cutoffReached(episode)))) {
                discard();
                throw systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin.framework(
                        ZLinkFrameworkErrorKind.NOT_FOUND,
                        "The resolved owner no longer owns this incarnation.");
            }
            if (!application) return;
            if (paused()) throw new OwnerLeaseAdmissionClosedException();
            if (isOwnerAdmissionOpenCore()) return;
            if (episode != null && episode.records.containsKey(this) && storeOutageRetention)
                return;
            throw new OwnerLeaseAdmissionClosedException();
        }

        /** Definitive stale/fence results and the outage latch share this terminal path. */
        public void discard() {
            discardCore();
        }

        private void discardCore() {
            Runnable action = cleanup.getAndSet(null);
            if (action == null) return;
            StoreOutage episode = outage;
            Preservation record = episode == null ? null : episode.records.get(this);
            if (record == null) close();
            else
                record.resumed.completeExceptionally(
                        new CancellationException("preserved incarnation discarded"));
            resumeCore();
            action.run();
        }

        @Override
        public void close() {
            if (ZLinkStateLane.current() == stateLane) closeCore();
            else
                stateLane.runAsync(
                        () -> {
                            closeCore();
                            return null;
                        });
        }

        private void closeCore() {
            executions.remove(key == null ? this : key, this);
            StoreOutage episode = outage;
            if (cleanup.get() != null && episode != null) {
                Preservation record = episode.records.remove(this);
                if (record != null)
                    record.resumed.completeExceptionally(
                            new CancellationException("incarnation closed"));
            }
        }
    }

    public Execution execution(BooleanSupplier steady, Runnable cleanup) {
        return execution(
                (String) null,
                steady,
                ignored -> CompletableFuture.completedFuture(null),
                () -> null,
                cleanup);
    }

    public Execution execution(
            String key,
            BooleanSupplier steady,
            Function<ZLinkAuthoritySnapshot, CompletionStage<Void>> apply,
            Supplier<String> parent,
            Runnable cleanup) {
        return execution(() -> key, steady, apply, parent, cleanup);
    }

    public Execution execution(
            Supplier<String> authorityKey,
            BooleanSupplier steady,
            Function<ZLinkAuthoritySnapshot, CompletionStage<Void>> apply,
            Supplier<String> parent,
            Runnable cleanup) {
        return new Execution(
                Objects.requireNonNull(authorityKey),
                Objects.requireNonNull(steady),
                Objects.requireNonNull(apply),
                Objects.requireNonNull(parent),
                Objects.requireNonNull(cleanup));
    }

    public CompletionStage<Void> confirmAuthority(String key, ZLinkAuthorityStored stored) {
        return stateLane.runAsync(
                () -> {
                    Execution execution = executions.get(key);
                    if (execution != null
                            && execution.cleanup.get() != null
                            && (outage == null || !outage.records.containsKey(execution)))
                        execution.confirmed = snapshot(stored);
                    return null;
                });
    }

    private static final class Preservation {
        final Execution execution;
        final ZLinkAuthoritySnapshot authority;
        final CompletableFuture<Void> resumed = new CompletableFuture<>();
        CompletionStage<Void> operation;

        Preservation(Execution execution) {
            this.execution = execution;
            authority = execution.confirmed;
        }
    }

    private static final class StoreOutage {
        final ZLinkLocationOwnerToken token;
        final long observedAtNanos;
        final long cutoffNanos;
        volatile boolean discarded;
        // This owner index is the sole retention/recovery decision record read by each gate.
        final java.util.Map<Execution, Preservation> records = new ConcurrentHashMap<>();
        volatile CompletionStage<Void> publication;
        CompletableFuture<Void> recovery;
        final Map<ZLinkMeshNodeDescriptorKey, DescriptorPublication> descriptors =
                new ConcurrentHashMap<>();

        StoreOutage(ZLinkLocationOwnerToken token, long observedAtNanos, long cutoffNanos) {
            this.token = token;
            this.observedAtNanos = observedAtNanos;
            this.cutoffNanos = cutoffNanos;
        }
    }

    private void observeStoreCommunicationFailureCore(Throwable failure) {
        if (unwrap(failure) instanceof CancellationException) return;
        executionOpen = false;
        if (!started || outage != null || lastSuccessfulLeaseRequestStartedNanos == null) return;
        long cutoff =
                storeOutageRetention
                        ? saturatingAdd(
                                lastSuccessfulLeaseRequestStartedNanos,
                                ownerLeaseTtl
                                        .plus(ownerLeaseFencingMargin.multipliedBy(2))
                                        .toNanos())
                        : ownerAdmissionDeadlineNanos;
        StoreOutage episode = new StoreOutage(ownerToken, monotonicNow.getAsLong(), cutoff);
        for (Execution execution : executions.values()) {
            if (execution.steady.getAsBoolean())
                episode.records.put(execution, new Preservation(execution));
        }
        outage = episode;
        if (latchOutageCore(episode)) return;
        outageDeadlineTask =
                heartbeatExecutor.schedule(
                        () ->
                                stateLane.runAsync(
                                        () -> {
                                            if (outage == episode) latchOutageCore(episode);
                                            return null;
                                        }),
                        cutoff - monotonicNow.getAsLong(),
                        TimeUnit.NANOSECONDS);
    }

    private boolean cutoffReached(StoreOutage episode) {
        return monotonicNow.getAsLong() - episode.cutoffNanos >= 0L;
    }

    private boolean latchOutageCore(StoreOutage episode) {
        if (!episode.discarded && cutoffReached(episode)) {
            discardOutageCore(episode);
        }
        if (episode.discarded) executionOpen = isOwnerAdmissionOpenCore();
        return episode.discarded;
    }

    private void discardOutageCore(StoreOutage episode) {
        if (episode.discarded) return;
        episode.discarded = true;
        for (Preservation record : episode.records.values()) record.execution.discardCore();
    }

    private void cancelOutageCore() {
        StoreOutage episode = outage;
        cancel(outageDeadlineTask);
        outageDeadlineTask = null;
        if (episode == null) return;
        executionOpen = false;
        outage = null;
        episode.discarded = true;
        for (Preservation record : episode.records.values()) record.resumed.cancel(false);
        episode.records.clear();
    }

    private boolean discardChangedOwnerLeaseCore(
            StoreOutage episode,
            ZLinkLocationOwnerToken expected,
            ZLinkOwnerLeaseReadResult exact) {
        if (!(exact instanceof ZLinkOwnerLeaseFound found) || expected.equals(found.token()))
            return false;
        executionOpen = false;
        ownerAdmissionRevoked = true;
        discardOutageCore(episode);
        return true;
    }

    private void observeSuccessfulLeaseCore(
            long operationStartedNanos, boolean renewal, StoreOutage requestedOutage) {
        if (outage != null) {
            if (latchOutageCore(outage)
                    || !renewal
                    || outage != requestedOutage
                    || !Objects.equals(outage.token, ownerToken)
                    || operationStartedNanos < outage.observedAtNanos) return;
            if (!outage.records.isEmpty()) return;
            cancel(outageDeadlineTask);
            outageDeadlineTask = null;
            outage = null;
        }
        executionOpen = isOwnerAdmissionOpenCore();
    }

    private <T> CompletionStage<T> observeLeaseOperation(Supplier<CompletionStage<T>> operation) {
        CompletionStage<T> request;
        try {
            request = operation.get();
        } catch (RuntimeException failure) {
            stateLane.runAsync(
                    () -> {
                        observeStoreCommunicationFailureCore(failure);
                        return null;
                    });
            throw failure;
        }
        return request.handle(
                        (result, failure) -> {
                            if (failure == null) return CompletableFuture.completedFuture(result);
                            return stateLane.<T>runAsync(
                                    () -> {
                                        observeStoreCommunicationFailureCore(failure);
                                        throw new CompletionException(unwrap(failure));
                                    });
                        })
                .thenCompose(stage -> stage);
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
     * Registers owner-scoped work after each successful renewal. True permits skipping publication
     * when admission was already open; false starts or continues unfinished owner recovery.
     */
    public void setOwnerLeaseRecoveryListener(Function<Boolean, CompletionStage<Void>> listener) {
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
        ensureOwnerAdmissionOpenCore();
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
                            ownerLeaseRecoveryOperation = null;
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
        cancel(inStateLane(this::startupCompletion));
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
                            cancelOutageCore();
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
                                    shouldStop,
                                    heartbeat,
                                    token,
                                    lifecycle,
                                    completion,
                                    ownerLeaseRecoveryOperation);
                        });
        cancel(state.heartbeat());
        cancel(state.recovery());
        if (state.lifecycle() == null) return CompletableFuture.completedFuture(null);
        cancel(state.lifecycle().completion());
        if (!state.shouldStop()) return state.completion();
        long deadlineNanos = deadlineNanos(deadline);
        ZLinkLocationOwnerToken token = state.token();
        ZLinkStoreCancellation cleanupCancellation =
                () -> deadlineNanos - monotonicNow.getAsLong() <= 0L;
        CompletionStage<Void> cleanup =
                state.lifecycle()
                        .cleanup()
                        .handle((ignored, failure) -> failure)
                        .thenCompose(
                                pendingFailure ->
                                        (token == null
                                                        ? CompletableFuture.completedFuture(0L)
                                                        : stores.unifiedStore()
                                                                .removeAllByOwner(
                                                                        token, cleanupCancellation))
                                                .thenCompose(
                                                        ignored ->
                                                                token == null
                                                                        ? CompletableFuture
                                                                                .completedFuture(
                                                                                        null)
                                                                        : stores.ownerLeaseStore()
                                                                                .releaseOwnerLease(
                                                                                        token,
                                                                                        cleanupCancellation)
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
                                                                                executionOpen =
                                                                                        false;
                                                                                ownerToken = null;
                                                                                ownerAdmissionRevoked =
                                                                                        true;
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
                                                            if (cleanupCancellation
                                                                    .isCancellationRequested())
                                                                throw new CompletionException(
                                                                        new TimeoutException(
                                                                                "host shutdown deadline expired"));
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

    private long deadlineNanos(Instant deadline) {
        Duration remaining = Duration.between(Instant.now(), deadline);
        return saturatingAdd(monotonicNow.getAsLong(), Math.max(0L, remaining.toNanos()));
    }

    public CompletionStage<Boolean> renewOwnerLeaseOnce() {
        return stateLane
                .runAsync(() -> ownerLeaseRenewalCore(startupCompletion()))
                .thenCompose(Supplier::get);
    }

    private Supplier<CompletionStage<Boolean>> ownerLeaseRenewalCore(
            CompletableFuture<Void> lifecycle) {
        RenewState state = new RenewState(nodeRid, ownerToken, outage);
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
                            .thenCompose(
                                    ignored -> republishAfterOwnerLeaseRecovery(lifecycle, false))
                            .thenApply(ignored -> true),
                    lifecycle);
        }
        long operationStartedNanos = monotonicNow.getAsLong();
        CompletionStage<Boolean> renewal =
                observeLeaseOperation(
                                () ->
                                        stores.ownerLeaseStore()
                                                .renewOwnerLease(token, ownerLeaseTtl))
                        .handle(
                                (result, failure) ->
                                        stateLane.<Supplier<CompletionStage<Boolean>>>runAsync(
                                                () -> {
                                                    if (!started
                                                            || !ownsStartupCore(lifecycle)
                                                            || !token.equals(ownerToken)) {
                                                        return () ->
                                                                CompletableFuture.completedFuture(
                                                                        false);
                                                    }
                                                    ownerLeaseOperationStartedNanos =
                                                            operationStartedNanos;
                                                    if (failure != null) {
                                                        recordRenewalFailureCore(
                                                                failureMessage(failure));
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
                                                        // A pre-outage response cannot establish
                                                        // recovery of this episode.
                                                        if (outage != null
                                                                && state.outage() != outage)
                                                            return () ->
                                                                    CompletableFuture
                                                                            .completedFuture(false);
                                                        boolean admissionWasOpen =
                                                                isOwnerAdmissionOpenCore();
                                                        recordSuccessfulRenewalCore(
                                                                renewed.leaseExpiresAt(),
                                                                renewed.storeNow(),
                                                                operationStartedNanos);
                                                        observeSuccessfulLeaseCore(
                                                                operationStartedNanos,
                                                                true,
                                                                state.outage());
                                                        var recovery =
                                                                prepareRecoveryCore(
                                                                        lifecycle,
                                                                        admissionWasOpen);
                                                        return () ->
                                                                recovery.get()
                                                                        .thenApply(ignored -> true);
                                                    }
                                                    recordRenewalFailureCore(
                                                            "owner lease renewal was stale");
                                                    executionOpen = false;
                                                    if (outage != null) {
                                                        StoreOutage episode = outage;
                                                        long reclaimAfter =
                                                                ownerLeaseReclaimAfterCore();
                                                        return () ->
                                                                reclaimExpiredOwnerLease(
                                                                        token,
                                                                        episode,
                                                                        reclaimAfter,
                                                                        lifecycle);
                                                    }
                                                    ownerAdmissionRevoked = true;
                                                    return () ->
                                                            claimOwnerLease(lifecycle)
                                                                    .thenCompose(
                                                                            ignored ->
                                                                                    republishAfterOwnerLeaseRecovery(
                                                                                            lifecycle,
                                                                                            false))
                                                                    .thenApply(ignored -> true);
                                                }))
                        .thenCompose(stage -> stage)
                        .thenCompose(Supplier::get);
        return reportRenewalFailure(renewal, lifecycle);
    }

    private long ownerLeaseReclaimAfterCore() {
        // Admission is lease lifetime minus margin; reclaim needs lifetime plus margin.
        return saturatingAdd(
                ownerAdmissionDeadlineNanos, ownerLeaseFencingMargin.multipliedBy(2).toNanos());
    }

    private CompletionStage<Boolean> reclaimExpiredOwnerLease(
            ZLinkLocationOwnerToken token,
            StoreOutage episode,
            long reclaimAfter,
            CompletableFuture<Void> lifecycle) {
        if (monotonicNow.getAsLong() - reclaimAfter < 0L)
            return CompletableFuture.completedFuture(false);
        long deadline = saturatingAdd(monotonicNow.getAsLong(), ownerLeaseRenewTimeout.toNanos());
        return observeLeaseOperation(
                        () ->
                                withinDeadline(
                                        () -> stores.ownerLeaseStore().readOwnerLease(ownerId),
                                        deadline))
                .thenCompose(
                        exact ->
                                stateLane.<Supplier<CompletionStage<Boolean>>>runAsync(
                                        () -> {
                                            if (!started
                                                    || !ownsStartupCore(lifecycle)
                                                    || !token.equals(ownerToken)
                                                    || outage != episode)
                                                return () ->
                                                        CompletableFuture.completedFuture(false);
                                            if (discardChangedOwnerLeaseCore(episode, token, exact))
                                                return () ->
                                                        CompletableFuture.completedFuture(false);
                                            // A concurrent successful renew may have extended this
                                            // token while the read was pending.
                                            if (monotonicNow.getAsLong()
                                                            - ownerLeaseReclaimAfterCore()
                                                    < 0L)
                                                return () ->
                                                        CompletableFuture.completedFuture(false);
                                            boolean expired =
                                                    exact instanceof ZLinkOwnerLeaseMissing
                                                            || exact
                                                                            instanceof
                                                                            ZLinkOwnerLeaseFound
                                                                                    found
                                                                    && !found.leaseExpiresAt()
                                                                            .isAfter(
                                                                                    found
                                                                                            .storeNow());
                                            if (!expired)
                                                return () ->
                                                        CompletableFuture.completedFuture(false);
                                            return () ->
                                                    claimOwnerLease(lifecycle, deadline)
                                                            .thenCompose(
                                                                    ignored ->
                                                                            recoverAfterOwnerLease(
                                                                                    lifecycle,
                                                                                    false))
                                                            .thenApply(ignored -> true);
                                        }))
                .thenCompose(Supplier::get);
    }

    private CompletionStage<Void> recoverAfterOwnerLease(
            CompletableFuture<Void> lifecycle, boolean admissionWasOpen) {
        return stateLane
                .runAsync(() -> prepareRecoveryCore(lifecycle, admissionWasOpen))
                .thenCompose(Supplier::get);
    }

    private Supplier<CompletionStage<Void>> prepareRecoveryCore(
            CompletableFuture<Void> lifecycle, boolean admissionWasOpen) {
        StoreOutage episode = outage;
        if (episode == null)
            return () -> republishAfterOwnerLeaseRecovery(lifecycle, admissionWasOpen);
        if (episode.recovery != null) {
            var running = episode.recovery;
            return () -> running;
        }
        var completion = new CompletableFuture<Void>();
        episode.recovery = completion;
        // Publish the pause before leaving the lane that installed the recovered lease.
        episode.publication = CompletableFuture.completedFuture(null);
        var pending = List.copyOf(episode.records.values());
        ZLinkLocationOwnerToken owner = ownerToken;
        boolean sameToken = owner.equals(episode.token);
        return () -> {
            var publication = new CompletableFuture<Void>();
            episode.publication = publication;
            republishAfterOwnerLeaseRecovery(
                            lifecycle,
                            sameToken && admissionWasOpen,
                            () ->
                                    launchRecovery(
                                            episode,
                                            pending,
                                            owner,
                                            sameToken,
                                            publication,
                                            completion))
                    .whenComplete(
                            (ignored, failure) -> {
                                if (failure == null) publication.complete(null);
                                else publication.completeExceptionally(unwrap(failure));
                            });
            return completion;
        };
    }

    private void launchRecovery(
            StoreOutage episode,
            List<Preservation> pending,
            ZLinkLocationOwnerToken owner,
            boolean sameToken,
            CompletionStage<Void> publication,
            CompletableFuture<Void> completion) {
        var descriptors =
                new HashMap<String, CompletionStage<Map<RoutingId, ZLinkMeshNodeDescriptor>>>();
        if (sameToken) {
            for (Preservation record : pending) {
                if (record.authority == null) continue;
                String mesh = record.authority.allocation().descriptor().meshName();
                descriptors.computeIfAbsent(
                        mesh,
                        ignored ->
                                withinDeadline(
                                                () ->
                                                        ZLinkStoreLocationResolvers.listMeshNodes(
                                                                locationStore(), mesh),
                                                saturatingAdd(
                                                        monotonicNow.getAsLong(),
                                                        ownerLeaseRenewTimeout.toNanos()))
                                        .thenApply(
                                                nodes -> {
                                                    var byRid =
                                                            new HashMap<
                                                                    RoutingId,
                                                                    ZLinkMeshNodeDescriptor>();
                                                    for (var node : nodes)
                                                        byRid.put(node.rid(), node);
                                                    return byRid;
                                                }));
            }
        }
        var attempts = new ArrayList<CompletableFuture<Void>>();
        for (Preservation record : pending) {
            CompletionStage<Void> descriptor = publication;
            CompletionStage<Void> submitted = CompletableFuture.completedFuture(null);
            if (!sameToken && record.authority != null) {
                DescriptorPublication own =
                        episode.descriptors.get(record.authority.allocation().descriptor());
                if (own != null) {
                    descriptor = own.confirmed();
                    submitted = own.submitted();
                }
            }
            if (sameToken && record.authority != null) {
                var preserved = record.authority;
                descriptor =
                        descriptors
                                .get(preserved.allocation().descriptor().meshName())
                                .thenAccept(
                                        nodes -> {
                                            var node =
                                                    nodes.get(
                                                            preserved
                                                                    .allocation()
                                                                    .descriptor()
                                                                    .rid());
                                            if (node == null
                                                    || node.lifecycleGeneration()
                                                            != preserved
                                                                    .allocation()
                                                                    .descriptorLifecycleGeneration()
                                                    || !node.ownerId().equals(owner.ownerId())
                                                    || node.leaseGeneration()
                                                            != owner.leaseGeneration())
                                                throw new RecoveryRejectedException(
                                                        "Preserved descriptor changed");
                                        });
            }
            CompletionStage<Void> confirmation = descriptor;
            attempts.add(
                    submitted
                            .handle(
                                    (ignored, failure) -> {
                                        if (failure == null)
                                            return recoverRecord(
                                                    episode, record, owner, confirmation);
                                        return stateLane
                                                .runAsync(
                                                        () -> {
                                                            if (!unconfirmedRecovery(failure))
                                                                record.execution.discardCore();
                                                            return null;
                                                        })
                                                .thenCompose(
                                                        done ->
                                                                cleanupRecord(
                                                                        episode, record, owner));
                                    })
                            .thenCompose(Function.identity())
                            .toCompletableFuture());
        }
        CompletableFuture.allOf(attempts.toArray(CompletableFuture[]::new))
                .handle(
                        (ignored, failure) ->
                                stateLane.runAsync(
                                        () -> {
                                            if (episode.recovery == completion)
                                                episode.recovery = null;
                                            finishRecoveryCore(episode);
                                            if (failure != null)
                                                throw new CompletionException(unwrap(failure));
                                            return null;
                                        }))
                .thenCompose(Function.identity())
                .whenComplete(
                        (ignored, failure) -> {
                            if (failure == null) completion.complete(null);
                            else completion.completeExceptionally(unwrap(failure));
                        });
    }

    private CompletionStage<Void> recoverRecord(
            StoreOutage episode,
            Preservation record,
            ZLinkLocationOwnerToken owner,
            CompletionStage<Void> descriptor) {
        return stateLane
                .<Supplier<CompletionStage<Void>>>runAsync(
                        () -> {
                            if (record.operation != null
                                    || record.execution.key == null && !owner.equals(episode.token))
                                return () -> CompletableFuture.completedFuture(null);
                            var completion = new CompletableFuture<Void>();
                            record.operation = completion;
                            return () ->
                                    confirmRecovery(episode, record, owner, descriptor)
                                            .handle(
                                                    (snapshot, failure) ->
                                                            stateLane.runAsync(
                                                                    () ->
                                                                            settleRecoveryCore(
                                                                                    episode,
                                                                                    record,
                                                                                    owner,
                                                                                    snapshot,
                                                                                    failure,
                                                                                    completion)))
                                            .thenCompose(Function.identity())
                                            .thenCompose(Supplier::get)
                                            .whenComplete(
                                                    (ignored, failure) -> {
                                                        if (failure != null)
                                                            completeRecord(
                                                                    record, completion, failure);
                                                    });
                        })
                .thenCompose(Supplier::get);
    }

    private CompletionStage<ZLinkAuthoritySnapshot> confirmRecovery(
            StoreOutage episode,
            Preservation record,
            ZLinkLocationOwnerToken owner,
            CompletionStage<Void> descriptor) {
        return CompletableFuture.completedFuture(null)
                .thenCompose(
                        ignored ->
                                record.execution.cleanup.get() == null
                                        ? CompletableFuture.<ZLinkAuthoritySnapshot>completedFuture(
                                                null)
                                        : recoverAuthority(episode, record, owner))
                .thenCompose(
                        snapshot ->
                                stateLane
                                        .runAsync(
                                                () -> {
                                                    // Late Store success is evidence for cleanup,
                                                    // never permission to reopen.
                                                    if (episode.discarded || cutoffReached(episode))
                                                        record.execution.discardCore();
                                                    return record.execution.cleanup.get() != null;
                                                })
                                        .thenCompose(
                                                alive ->
                                                        !alive
                                                                ? CompletableFuture.completedFuture(
                                                                        snapshot)
                                                                : descriptor.thenApply(
                                                                        ignored -> snapshot)));
    }

    private Supplier<CompletionStage<Void>> settleRecoveryCore(
            StoreOutage episode,
            Preservation record,
            ZLinkLocationOwnerToken owner,
            ZLinkAuthoritySnapshot snapshot,
            Throwable failure,
            CompletableFuture<Void> completion) {
        if (failure != null && !unconfirmedRecovery(failure)) record.execution.discardCore();
        Supplier<CompletionStage<Void>> continuation = () -> cleanupRecord(episode, record, owner);
        if (failure == null && record.execution.cleanup.get() != null) {
            if (record.execution.key != null && snapshot == null) record.execution.discardCore();
            else {
                String parentKey = record.execution.parent.get();
                Execution parent = parentKey == null ? null : executions.get(parentKey);
                Preservation parentRecord = parent == null ? null : episode.records.get(parent);
                if (parentKey != null && (parent == null || parent.cleanup.get() == null))
                    record.execution.discardCore();
                else if (parentRecord != null && !parentRecord.resumed.isDone()) {
                    // Parent confirmation must leave the renew heartbeat free to advance.
                    ZLinkStateLane.publishOutsideLane(
                            parentRecord.resumed,
                            (ignored, error) -> {
                                if (error != null) record.execution.discard();
                                finishRecoveredRecord(episode, record, snapshot, owner)
                                        .whenComplete(
                                                (done, terminal) ->
                                                        completeRecord(
                                                                record, completion, terminal));
                            });
                    return () -> CompletableFuture.completedFuture(null);
                } else continuation = () -> finishRecoveredRecord(episode, record, snapshot, owner);
            }
        }
        Supplier<CompletionStage<Void>> work = continuation;
        return () ->
                work.get()
                        .whenComplete(
                                (done, terminal) -> completeRecord(record, completion, terminal));
    }

    private void completeRecord(
            Preservation record, CompletableFuture<Void> completion, Throwable failure) {
        stateLane
                .runAsync(
                        () -> {
                            if (record.operation == completion) record.operation = null;
                            return null;
                        })
                .whenComplete(
                        (ignored, laneFailure) -> {
                            Throwable terminal = laneFailure == null ? failure : laneFailure;
                            if (terminal == null) completion.complete(null);
                            else completion.completeExceptionally(unwrap(terminal));
                        });
    }

    private CompletionStage<Void> finishRecoveredRecord(
            StoreOutage episode,
            Preservation record,
            ZLinkAuthoritySnapshot snapshot,
            ZLinkLocationOwnerToken owner) {
        return stateLane
                .runAsync(
                        () -> {
                            if (!started
                                    || !owner.equals(ownerToken)
                                    || outage != episode
                                    || episode.discarded
                                    || cutoffReached(episode)
                                    || record.execution.cleanup.get() == null
                                    || record.resumed.isCompletedExceptionally()) {
                                record.execution.discardCore();
                                return false;
                            }
                            return true;
                        })
                .thenCompose(
                        allowed ->
                                !allowed || snapshot == null
                                        ? CompletableFuture.completedFuture(null)
                                        : record.execution.apply.apply(snapshot))
                .thenCompose(
                        ignored ->
                                stateLane.runAsync(
                                        () -> {
                                            if (started
                                                    && owner.equals(ownerToken)
                                                    && outage == episode
                                                    && !episode.discarded
                                                    && !cutoffReached(episode)
                                                    && record.execution.cleanup.get() != null
                                                    && !record.resumed.isCompletedExceptionally()) {
                                                if (snapshot != null)
                                                    record.execution.confirmed = snapshot;
                                                episode.records.remove(record.execution, record);
                                                record.resumed.complete(null);
                                                finishRecoveryCore(episode);
                                                return (Runnable) record.execution::resumeCore;
                                            }
                                            record.execution.discardCore();
                                            return (Runnable) () -> {};
                                        }))
                .thenAccept(Runnable::run)
                .thenCompose(ignored -> cleanupRecord(episode, record, owner));
    }

    private void finishRecoveryCore(StoreOutage episode) {
        if (outage != episode
                || episode.discarded
                || !episode.records.isEmpty()
                || cutoffReached(episode)) return;
        cancel(outageDeadlineTask);
        outageDeadlineTask = null;
        outage = null;
        executionOpen = isOwnerAdmissionOpenCore();
    }

    private CompletionStage<ZLinkAuthoritySnapshot> recoverAuthority(
            StoreOutage episode, Preservation record, ZLinkLocationOwnerToken owner) {
        if (record.execution.key == null) return CompletableFuture.completedFuture(null);
        ZLinkAuthoritySnapshot preserved = record.authority;
        if (preserved == null)
            return CompletableFuture.failedFuture(
                    new RecoveryRejectedException("No confirmed authority before the outage"));
        ZLinkAuthorityPut put = ZLinkAuthorityPut.preserve(preserved, owner);
        long deadline = saturatingAdd(monotonicNow.getAsLong(), ownerLeaseRenewTimeout.toNanos());
        return CompletableFuture.completedFuture(null)
                .thenCompose(
                        ignored ->
                                withinDeadline(
                                        () ->
                                                locationStore()
                                                        .read(record.execution.key, () -> false),
                                        deadline))
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot current)
                                    || !preserved.sameIncarnation(current)
                                    || !preserved.allocation().equals(current.allocation())
                                    || !current.steadyReady())
                                return CompletableFuture.failedFuture(
                                        new RecoveryRejectedException(
                                                "Preserved authority changed"));
                            if (owner.equals(episode.token)) {
                                if (!preserved.storeVersion().equals(current.storeVersion())
                                        || current.ownerLeaseGeneration()
                                                != owner.leaseGeneration())
                                    return CompletableFuture.failedFuture(
                                            new RecoveryRejectedException(
                                                    "Preserved authority fence changed"));
                                return CompletableFuture.completedFuture(current);
                            }
                            // §10: exact intended bytes confirm a previous indeterminate Preserve.
                            if (matchesRecovery(preserved, current, owner, put))
                                return CompletableFuture.completedFuture(current);
                            long started = monotonicNow.getAsLong();
                            if (record.execution.cleanup.get() == null
                                    || record.resumed.isCompletedExceptionally()
                                    || episode.discarded)
                                return CompletableFuture.failedFuture(
                                        new RecoveryRejectedException(
                                                "Preserved incarnation discarded"));
                            if (saturatingAdd(started, ownerLeaseRenewTimeout.toNanos())
                                            - episode.cutoffNanos
                                    > 0L)
                                return CompletableFuture.failedFuture(
                                        new RecoveryUnconfirmedException(
                                                new TimeoutException(
                                                        "Preserve cannot be submitted before the original cutoff")));
                            if (!put.matchesPreserved(current))
                                return CompletableFuture.failedFuture(
                                        new RecoveryRejectedException(
                                                "Preserve lost its expected version"));
                            long writeDeadline =
                                    saturatingAdd(started, ownerLeaseRenewTimeout.toNanos());
                            return withinDeadline(
                                            () ->
                                                    locationStore()
                                                            .compareExchange(
                                                                    record.execution.key,
                                                                    new ZLinkAuthorityExpectFound(
                                                                            preserved
                                                                                    .storeVersion()),
                                                                    put,
                                                                    () -> false),
                                            writeDeadline)
                                    .handle(
                                            (result, failure) -> {
                                                if (failure == null)
                                                    return result
                                                                    instanceof
                                                                    ZLinkAuthorityStored stored
                                                            ? CompletableFuture.completedFuture(
                                                                    snapshot(stored))
                                                            : CompletableFuture
                                                                    .<ZLinkAuthoritySnapshot>
                                                                            failedFuture(
                                                                                    new RecoveryRejectedException(
                                                                                            "Preserve conflicted"));
                                                return withinDeadline(
                                                                () ->
                                                                        locationStore()
                                                                                .read(
                                                                                        record.execution
                                                                                                .key,
                                                                                        () ->
                                                                                                false),
                                                                saturatingAdd(
                                                                        monotonicNow.getAsLong(),
                                                                        ownerLeaseRenewTimeout
                                                                                .toNanos()))
                                                        .thenCompose(
                                                                exact -> {
                                                                    if (exact
                                                                            instanceof
                                                                            ZLinkAuthoritySnapshot
                                                                                    found) {
                                                                        if (matchesRecovery(
                                                                                preserved, found,
                                                                                owner, put))
                                                                            return CompletableFuture
                                                                                    .completedFuture(
                                                                                            found);
                                                                        if (put.matchesPreserved(
                                                                                found))
                                                                            return CompletableFuture
                                                                                    .failedFuture(
                                                                                            new RecoveryUnconfirmedException(
                                                                                                    unwrap(
                                                                                                            failure)));
                                                                    }
                                                                    return CompletableFuture
                                                                            .failedFuture(
                                                                                    new RecoveryRejectedException(
                                                                                            "Preserve confirmation differs"));
                                                                });
                                            })
                                    .thenCompose(Function.identity());
                        })
                .exceptionallyCompose(
                        failure -> {
                            Throwable error = unwrap(failure);
                            return CompletableFuture.failedFuture(
                                    error instanceof RecoveryRejectedException
                                            ? error
                                            : new RecoveryUnconfirmedException(error));
                        });
    }

    private static boolean matchesRecovery(
            ZLinkAuthoritySnapshot preserved,
            ZLinkAuthoritySnapshot current,
            ZLinkLocationOwnerToken owner,
            ZLinkAuthorityPut put) {
        return preserved.sameIncarnation(current)
                && preserved.allocation().equals(current.allocation())
                && current.ownerLeaseGeneration() == owner.leaseGeneration()
                && java.util.Arrays.equals(current.payload(), put.payload());
    }

    private CompletionStage<Void> cleanupRecord(
            StoreOutage episode, Preservation record, ZLinkLocationOwnerToken owner) {
        if (record.execution.cleanup.get() != null) return CompletableFuture.completedFuture(null);
        CompletionStage<Boolean> cleanup =
                record.authority == null
                        ? CompletableFuture.completedFuture(true)
                        : locationStore()
                                .cleanupDiscarded(
                                        record.execution.key, record.authority, owner, () -> false);
        return cleanup.thenCompose(
                done ->
                        stateLane.runAsync(
                                () -> {
                                    if (done) {
                                        episode.records.remove(record.execution, record);
                                        executions.remove(
                                                record.execution.key == null
                                                        ? record.execution
                                                        : record.execution.key,
                                                record.execution);
                                        finishRecoveryCore(episode);
                                    }
                                    return null;
                                }));
    }

    private static boolean unconfirmedRecovery(Throwable failure) {
        Throwable error = unwrap(failure);
        if (error instanceof ZLinkFrameworkException frameworkFailure)
            return frameworkFailure.kind() == ZLinkFrameworkErrorKind.UNAVAILABLE;
        return error instanceof TimeoutException
                || error instanceof CancellationException
                || error instanceof RecoveryUnconfirmedException
                || error
                        instanceof
                        ZLinkObjectServerDescriptorPublisher.UnconfirmedPublicationException
                || error instanceof ZLinkLocationStoreFailure;
    }

    private static ZLinkAuthoritySnapshot snapshot(ZLinkAuthorityStored stored) {
        return new ZLinkAuthoritySnapshot(
                stored.storeVersion(),
                stored.payload(),
                stored.objectGeneration(),
                stored.authorityOwnerGeneration(),
                stored.ownerId(),
                stored.ownerLeaseGeneration(),
                stored.allocation(),
                stored.storeNow());
    }

    private static final class RecoveryUnconfirmedException extends RuntimeException {
        RecoveryUnconfirmedException(Throwable cause) {
            super(cause);
        }
    }

    private static final class RecoveryRejectedException extends IllegalStateException {
        RecoveryRejectedException(String message) {
            super(message);
        }
    }

    /** Records renewal failures only for the lifecycle that requested them. */
    private CompletionStage<Boolean> reportRenewalFailure(
            CompletionStage<Boolean> renewal, CompletableFuture<Void> lifecycle) {
        return renewal.exceptionallyCompose(
                failure ->
                        stateLane.runAsync(
                                () -> {
                                    if (started && ownsStartupCore(lifecycle)) {
                                        recordRenewalFailureCore(failureMessage(failure));
                                    }
                                    return false;
                                }));
    }

    private void recordRenewalFailureCore(String message) {
        recordFailureCore(message);
        recordLeaseFailure();
    }

    private boolean ownsStartupCore(CompletableFuture<Void> completion) {
        return startupCompletion() == completion;
    }

    private CompletionStage<Void> claimOwnerLease(CompletableFuture<Void> lifecycle) {
        long deadlineNanos =
                saturatingAdd(monotonicNow.getAsLong(), ownerLeaseRenewTimeout.toNanos());
        return claimOwnerLease(lifecycle, deadlineNanos);
    }

    private CompletionStage<Void> claimOwnerLease(
            CompletableFuture<Void> lifecycle, long deadlineNanos) {
        StoreOutage episode = outage;
        ZLinkLocationOwnerToken previousToken = ownerToken;
        long operationStartedNanos = monotonicNow.getAsLong();
        if (deadlineNanos - operationStartedNanos <= 0L)
            return CompletableFuture.failedFuture(
                    new TimeoutException("owner lease operation timed out"));
        return resolveOwnerLeaseClaim(
                        deadlineNanos,
                        stores.ownerLeaseStore().claimOwnerLease(ownerId, ownerLeaseTtl),
                        episode,
                        previousToken)
                .handle(
                        (claimed, failure) ->
                                installOwnerLease(
                                        claimed, failure, operationStartedNanos, lifecycle))
                .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimed> resolveOwnerLeaseClaim(
            long deadlineNanos, CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation) {
        return resolveOwnerLeaseClaim(deadlineNanos, claimOperation, null, null);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimed> resolveOwnerLeaseClaim(
            long deadlineNanos,
            CompletionStage<ZLinkOwnerLeaseClaimResult> claimOperation,
            StoreOutage episode,
            ZLinkLocationOwnerToken previousToken) {
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
                                        ? confirmClaimAfterConflict(
                                                result, deadlineNanos, episode, previousToken)
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
            Throwable failure,
            long operationStartedNanos,
            CompletableFuture<Void> lifecycle) {
        return stateLane.runAsync(
                () -> {
                    if (!started || !ownsStartupCore(lifecycle)) {
                        throw new CancellationException("owner lease lifecycle ended");
                    }
                    ownerLeaseOperationStartedNanos = operationStartedNanos;
                    if (failure != null) throw new CompletionException(unwrap(failure));
                    recoveryPreviousOwnerToken = ownerToken;
                    installOwnerLeaseCore(claimed, operationStartedNanos);
                    return null;
                });
    }

    private void installOwnerLeaseCore(ZLinkOwnerLeaseClaimed claimed, long operationStartedNanos) {
        ownerToken = claimed.token();
        recordSuccessfulRenewalCore(
                claimed.leaseExpiresAt(), claimed.storeNow(), operationStartedNanos);
        observeSuccessfulLeaseCore(operationStartedNanos, false, null);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterConflict(
            ZLinkOwnerLeaseClaimResult conflict,
            long deadlineNanos,
            StoreOutage episode,
            ZLinkLocationOwnerToken previousToken) {
        return observeLeaseOperation(
                        () ->
                                withinDeadline(
                                        () -> stores.ownerLeaseStore().readOwnerLease(ownerId),
                                        deadlineNanos))
                .thenCompose(
                        exact ->
                                stateLane.runAsync(
                                        () -> {
                                            if (episode != null
                                                    && outage == episode
                                                    && previousToken.equals(ownerToken)
                                                    && discardChangedOwnerLeaseCore(
                                                            episode, previousToken, exact))
                                                throw new OwnerLeaseClaimRejectedException(
                                                        "owner lease changed during recovery");
                                            return exact;
                                        }))
                .handle(
                        (result, failure) ->
                                failure == null
                                                && result instanceof ZLinkOwnerLeaseFound found
                                                && ownerId.equals(found.token().ownerId())
                                        ? new ZLinkOwnerLeaseClaimed(
                                                found.token(),
                                                found.leaseExpiresAt(),
                                                found.storeNow())
                                        : conflict);
    }

    private CompletionStage<ZLinkOwnerLeaseClaimResult> confirmClaimAfterFailure(
            Throwable failure, long deadlineNanos) {
        return observeLeaseOperation(
                        () ->
                                withinDeadline(
                                        () -> stores.ownerLeaseStore().readOwnerLease(ownerId),
                                        deadlineNanos))
                .thenCompose(
                        result ->
                                result instanceof ZLinkOwnerLeaseFound found
                                                && ownerId.equals(found.token().ownerId())
                                        ? CompletableFuture.completedFuture(
                                                new ZLinkOwnerLeaseClaimed(
                                                        found.token(),
                                                        found.leaseExpiresAt(),
                                                        found.storeNow()))
                                        : CompletableFuture.failedFuture(failure));
    }

    /**
     * Bounds the complete descriptor repository operation, including confirmation reads, by the
     * existing renew timeout. The Store stage and its input remain alive after the caller's
     * deadline; only this wait completes, so late Store results can finish normally.
     */
    private record DescriptorPublication(
            CompletableFuture<Void> submitted, CompletableFuture<Void> confirmed) {}

    public <T> CompletionStage<T> writeDescriptor(
            ZLinkMeshNodeDescriptorKey key, Supplier<CompletionStage<T>> write) {
        StoreOutage episode = outage;
        if (episode == null) return writeDescriptor(write);
        var publication =
                new DescriptorPublication(new CompletableFuture<>(), new CompletableFuture<>());
        episode.descriptors.put(key, publication);
        var operation =
                writeDescriptor(
                        () -> {
                            try {
                                var request = write.get();
                                publication.submitted().complete(null);
                                return request;
                            } catch (RuntimeException failure) {
                                publication.submitted().completeExceptionally(failure);
                                throw failure;
                            }
                        });
        operation.whenComplete(
                (result, failure) -> {
                    if (failure != null) {
                        publication.submitted().completeExceptionally(unwrap(failure));
                        publication.confirmed().completeExceptionally(unwrap(failure));
                    } else publication.confirmed().complete(null);
                });
        return operation;
    }

    public <T> CompletionStage<T> writeDescriptor(Supplier<CompletionStage<T>> write) {
        long deadlineNanos =
                saturatingAdd(monotonicNow.getAsLong(), ownerLeaseRenewTimeout.toNanos());
        return stateLane
                .runAsync(() -> ownerLeaseRecoveryOperation)
                .thenCompose(
                        recovery -> {
                            if (recovery != null && recovery.isCompletedExceptionally()) {
                                return recovery.<T>thenApply(ignored -> null);
                            }
                            var completion =
                                    withinDeadline(write, deadlineNanos).toCompletableFuture();
                            if (recovery != null) {
                                recovery.whenComplete(
                                        (ignored, failure) -> {
                                            if (failure != null)
                                                completion.completeExceptionally(unwrap(failure));
                                        });
                            }
                            return completion;
                        });
    }

    private <T> CompletionStage<T> withinDeadline(
            CompletionStage<T> operation, long deadlineNanos) {
        CompletableFuture<T> completion = new CompletableFuture<>();
        operation.whenComplete(
                (result, failure) -> {
                    if (failure == null) completion.complete(result);
                    else completion.completeExceptionally(failure);
                });
        if (completion.isDone()) return completion;
        long remainingNanos = deadlineNanos - monotonicNow.getAsLong();
        if (remainingNanos <= 0L) {
            completion.completeExceptionally(
                    new TimeoutException("owner lease operation timed out"));
            return completion;
        }
        ScheduledFuture<?> timeout =
                heartbeatExecutor.schedule(
                        () ->
                                completion.completeExceptionally(
                                        new TimeoutException("owner lease operation timed out")),
                        remainingNanos,
                        TimeUnit.NANOSECONDS);
        completion.whenComplete((result, failure) -> timeout.cancel(false));
        return completion;
    }

    private <T> CompletionStage<T> withinDeadline(
            Supplier<CompletionStage<T>> operation, long deadlineNanos) {
        if (deadlineNanos - monotonicNow.getAsLong() <= 0L) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("owner lease operation timed out"));
        }
        return withinDeadline(operation.get(), deadlineNanos);
    }

    /**
     * Runs the same owner-scoped work at startup and after renewal, with one in-flight operation.
     */
    public CompletionStage<Void> publishWithStartupOwnerLease() {
        return stateLane
                .runAsync(() -> startupCompletion())
                .thenCompose(lifecycle -> republishAfterOwnerLeaseRecovery(lifecycle, false));
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery(
            CompletableFuture<Void> lifecycle, boolean admissionWasOpen) {
        return republishAfterOwnerLeaseRecovery(lifecycle, admissionWasOpen, () -> {});
    }

    private CompletionStage<Void> republishAfterOwnerLeaseRecovery(
            CompletableFuture<Void> lifecycle, boolean admissionWasOpen, Runnable submitted) {
        return stateLane
                .<Supplier<CompletionStage<Void>>>runAsync(
                        () -> {
                            if (!started
                                    || !ownsStartupCore(lifecycle)
                                    || ownerLeaseRecoveryListener == null) {
                                return () -> {
                                    submitted.run();
                                    return CompletableFuture.completedFuture(null);
                                };
                            }
                            if (ownerLeaseRecoveryOperation != null) {
                                var running = ownerLeaseRecoveryOperation;
                                return () -> {
                                    running.whenComplete((ignored, failure) -> submitted.run());
                                    return running;
                                };
                            }
                            var completion = new CompletableFuture<Void>();
                            ownerLeaseRecoveryOperation = completion;
                            var listener = ownerLeaseRecoveryListener;
                            StoreOutage recoveryEpisode = outage;
                            return () -> {
                                if (completion.isDone()) return completion;
                                var publication =
                                        CompletableFuture.<Void>completedFuture(null)
                                                .thenCompose(
                                                        ignored -> {
                                                            try {
                                                                return listener.apply(
                                                                        admissionWasOpen);
                                                            } finally {
                                                                submitted.run();
                                                            }
                                                        });
                                completion.whenComplete(
                                        (ignored, failure) -> {
                                            if (completion.isCancelled()) publication.cancel(false);
                                        });
                                publication
                                        .handle(
                                                (ignored, failure) ->
                                                        stateLane.runAsync(
                                                                () -> {
                                                                    if (failure != null
                                                                            && started
                                                                            && ownsStartupCore(
                                                                                    lifecycle)) {
                                                                        if (recoveryEpisode
                                                                                == null) {
                                                                            executionOpen = false;
                                                                            ownerAdmissionRevoked =
                                                                                    true;
                                                                        } else
                                                                            executionOpen =
                                                                                    outage == null
                                                                                            && isOwnerAdmissionOpenCore();
                                                                    }
                                                                    if (ownerLeaseRecoveryOperation
                                                                            == completion) {
                                                                        ownerLeaseRecoveryOperation =
                                                                                null;
                                                                    }
                                                                    return failure;
                                                                }))
                                        .thenCompose(stage -> stage)
                                        .whenComplete(
                                                (failure, laneFailure) -> {
                                                    Throwable terminalFailure =
                                                            laneFailure == null
                                                                    ? failure
                                                                    : laneFailure;
                                                    if (terminalFailure == null)
                                                        completion.complete(null);
                                                    else
                                                        completion.completeExceptionally(
                                                                unwrap(terminalFailure));
                                                });
                                return completion;
                            };
                        })
                .thenCompose(Supplier::get);
    }

    private void attemptInitialOwnerLeaseClaim(StartupLifecycle lifecycle) {
        CompletableFuture<Void> completion = lifecycle.completion();
        boolean shouldClaim =
                inStateLane(() -> started && ownsStartupCore(completion) && !completion.isDone());
        if (!shouldClaim) {
            lifecycle.cleanup().complete(null);
            return;
        }
        long operationStartedNanos = monotonicNow.getAsLong();
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
                                                        cleanupDeadline != null
                                                                ? startupCancellationCleanup(
                                                                        cleanupDeadline,
                                                                        claimedToken,
                                                                        completion)
                                                                : CompletableFuture.completedFuture(
                                                                        null)))
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
                                        .thenCompose(Supplier::get));
    }

    /**
     * Applies the startup claim result to lane state and selects its completion outside the lane.
     */
    private Supplier<CompletionStage<Void>> installStartupClaimCore(
            CompletableFuture<Void> completion,
            ZLinkOwnerLeaseClaimed claimed,
            Throwable failure,
            long operationStartedNanos) {
        boolean ownsStartup = ownsStartupCore(completion);
        if (!ownsStartup || !started || completion.isCancelled()) {
            if (ownsStartup) {
                executionOpen = false;
                ownerToken = null;
                ownerAdmissionRevoked = true;
            }
            return () -> CompletableFuture.completedFuture(null);
        }
        ownerLeaseOperationStartedNanos = operationStartedNanos;
        if (failure == null) {
            installOwnerLeaseCore(claimed, operationStartedNanos);
        } else {
            recordFailureCore(failureMessage(failure));
            if (unwrap(failure) instanceof OwnerLeaseClaimRejectedException) {
                started = false;
                return () -> {
                    completion.completeExceptionally(unwrap(failure));
                    return CompletableFuture.completedFuture(null);
                };
            }
        }
        return () -> completeInitialClaim(completion).thenApply(ignored -> null);
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
        long scheduledAt = ownerLeaseOperationStartedNanos + heartbeatInterval.toNanos();
        long delay = Math.max(0L, scheduledAt - monotonicNow.getAsLong());
        CompletableFuture<Void> lifecycle = startupCompletion();
        heartbeatTask =
                heartbeatExecutor.schedule(
                        () -> renewOwnerLeaseOnHeartbeat(lifecycle, scheduledAt),
                        delay,
                        TimeUnit.NANOSECONDS);
    }

    private void cancelStartupClaim(CompletableFuture<Void> completion) {
        ScheduledFuture<?> heartbeat =
                inStateLane(
                        () -> {
                            if (!ownsStartupCore(completion)) {
                                return null;
                            }
                            started = false;
                            cancelOutageCore();
                            executionOpen = false;
                            ownerToken = null;
                            ownerAdmissionRevoked = true;
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
                                            if (result instanceof ZLinkOwnerLeaseFound found
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
                            cancelOutageCore();
                            ScheduledFuture<?> heartbeat = heartbeatTask;
                            heartbeatTask = null;
                            return new StopState(
                                    false,
                                    heartbeat,
                                    null,
                                    startupLifecycle,
                                    null,
                                    ownerLeaseRecoveryOperation);
                        });
        cancel(state.heartbeat());
        cancel(state.recovery());
        heartbeatExecutor.shutdownNow();
        heartbeatExecutor.close();
    }

    private void renewOwnerLeaseOnHeartbeat(CompletableFuture<Void> lifecycle, long scheduledAt) {
        long firedNanos = monotonicNow.getAsLong();
        stateLane
                .<Supplier<CompletionStage<Boolean>>>runAsync(
                        () -> {
                            if (!started || !ownsStartupCore(lifecycle)) {
                                return () -> CompletableFuture.completedFuture(false);
                            }
                            heartbeatTask = null;
                            Supplier<CompletionStage<Boolean>> renewal =
                                    ownerLeaseRenewalCore(lifecycle);
                            return () -> {
                                recordLeaseLateness(
                                        Duration.ofNanos(Math.max(0L, firedNanos - scheduledAt)));
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
        if (lastSuccessfulLeaseRequestStartedNanos == null
                || operationStartedNanos - lastSuccessfulLeaseRequestStartedNanos >= 0L) {
            lastSuccessfulLeaseRequestStartedNanos = operationStartedNanos;
        }
        ownerAdmissionDeadlineNanos =
                saturatingAdd(operationStartedNanos, admissionLifetime.toNanos());
        ownerAdmissionRevoked = false;
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
        executionOpen = false;
        lastError = message == null || message.isBlank() ? "owner lease operation failed" : message;
    }

    private static void cancel(Future<?> task) {
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
            CompletableFuture<Void> completion,
            CompletableFuture<Void> recovery) {}

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
            RoutingId nodeRid, ZLinkLocationOwnerToken token, StoreOutage outage) {}

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
                && !ownerAdmissionRevoked
                && ownerAdmissionDeadlineNanos != 0L
                && monotonicNow.getAsLong() - ownerAdmissionDeadlineNanos < 0L;
    }

    private Optional<ZLinkLocationOwnerToken> admittedOwnerTokenCore() {
        return isOwnerAdmissionOpenCore() ? Optional.of(ownerToken) : Optional.empty();
    }

    private void ensureOwnerAdmissionOpenCore() {
        if (!isOwnerAdmissionOpenCore()) {
            throw new OwnerLeaseAdmissionClosedException();
        }
    }

    private static final class OwnerLeaseAdmissionClosedException extends IllegalStateException
            implements ZLinkLocationStoreFailure {
        private OwnerLeaseAdmissionClosedException() {
            super("The owner lease admission deadline has expired.");
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
