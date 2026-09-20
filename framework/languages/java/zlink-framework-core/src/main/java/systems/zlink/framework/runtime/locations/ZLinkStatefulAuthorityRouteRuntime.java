package systems.zlink.framework.runtime.locations;
import java.util.Objects;
import java.util.concurrent.CompletionException;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;

import java.time.Duration;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanExpired;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

/**
 * Reconciles durable Spot authority into fenced raw routes. A full scan is
 * applied atomically; an expired scan keeps the previous route set.
 */
public final class ZLinkStatefulAuthorityRouteRuntime
    implements AutoCloseable {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    private final ZLinkLocationRepository store;
    private final ZLinkRelocationStore relocationStore;
    private final Map<String, ZLinkInternalMeshNode> meshNodes;
    private final Duration pollingInterval;
    private final Consumer<Throwable> reportFailure;
    private final ZLinkServiceAuthorityPayloadCodec payloadCodec =
        new ZLinkServiceAuthorityPayloadCodec();
    private final ScheduledExecutorService executor =
        Executors.newSingleThreadScheduledExecutor(
            Thread.ofVirtual()
                .name("zlink-jvm-authority-routes")
                .factory());
    private final AtomicBoolean inFlight = new AtomicBoolean();
    // Applied routes are a C2 set: remove, add, and replacement must remain
    // one ordered transition.
    private final ZLinkStateLane stateLane = new ZLinkStateLane();
    private final Map<String, Applied> applied = new HashMap<>();
    private volatile boolean started;
    private volatile boolean closed;

    public ZLinkStatefulAuthorityRouteRuntime(
        ZLinkLocationRepository store,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        Duration pollingInterval,
        Consumer<Throwable> reportFailure) {
        this(store, null, meshNodes, pollingInterval, reportFailure);
    }

    public ZLinkStatefulAuthorityRouteRuntime(
        ZLinkLocationRepository store,
        ZLinkRelocationStore relocationStore,
        Map<String, ZLinkInternalMeshNode> meshNodes,
        Duration pollingInterval,
        Consumer<Throwable> reportFailure) {
        this.store = Objects.requireNonNull(store, "store");
        this.relocationStore = relocationStore;
        this.meshNodes = Map.copyOf(
            Objects.requireNonNull(
                meshNodes, "meshNodes"));
        this.pollingInterval = Objects.requireNonNull(
            pollingInterval, "pollingInterval");
        if (pollingInterval.isNegative()
            || pollingInterval.isZero()) {
            throw new IllegalArgumentException(
                "pollingInterval must be positive");
        }
        this.reportFailure = Objects.requireNonNull(
            reportFailure, "reportFailure");
    }

    public CompletionStage<Void> start() {
        return reconcile()
            .thenRun(() -> {
                started = true;
                executor.scheduleWithFixedDelay(
                    this::poll,
                    pollingInterval.toMillis(),
                    pollingInterval.toMillis(),
                    TimeUnit.MILLISECONDS);
            });
    }

    public CompletionStage<Void> reconcile() {
        return scan(Optional.empty(), new HashMap<>())
            .thenCompose(this::recoverActivations)
            .thenAccept(next -> inStateLane(() -> {
                applyCore(next);
                return null;
            }));
    }

    private CompletionStage<Map<String, Applied>> scan(
        Optional<ZLinkAuthorityScanCursor> cursor,
        Map<String, Applied> routes) {
        return store.list(
                ZLinkAuthorityKeyCodec.spotPrefix(),
                cursor,
                1000,
                OPEN)
            .thenCompose(result -> {
                if (result instanceof ZLinkAuthorityScanExpired) {
                    return CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "authority scan expired"));
                }
                ZLinkAuthorityPage page =
                    (ZLinkAuthorityPage) result;
                for (var entry : page.items()) {
                    decode(entry.snapshot()).ifPresent(value ->
                        routes.put(entry.key(), value));
                }
                return page.nextCursor().isEmpty()
                    ? CompletableFuture.completedFuture(routes)
                    : scan(page.nextCursor(), routes);
            });
    }

    private Optional<Applied> decode(
        ZLinkAuthoritySnapshot snapshot) {
        if (snapshot.allocation().objectKind()
                    != ZLinkPlacementObjectKind.USER_SPOT
                && snapshot.allocation().objectKind()
                    != ZLinkPlacementObjectKind.INSTANCE_SPOT) {
            return Optional.empty();
        }
        return payloadCodec.decode(snapshot.payload())
            .filter(value -> {
                boolean ready =
                    snapshot.allocation().state()
                        == ZLinkPlacementAllocationState.ACTIVE
                    && value.state()
                        == ZLinkServiceAuthorityPayloadCodec.State.READY;
                boolean coldActivation =
                    snapshot.allocation().state()
                        == ZLinkPlacementAllocationState.PENDING
                    && value.instance().isPresent()
                    && value.state()
                        == ZLinkServiceAuthorityPayloadCodec.State.CREATING;
                return (ready || coldActivation)
                && value.ownerId().equals(snapshot.ownerId())
                && value.ownerLeaseGeneration()
                    == snapshot.ownerLeaseGeneration()
                && value.meshName().equals(
                    snapshot.allocation().descriptor().meshName())
                && value.nodeRid().equals(
                    snapshot.allocation().descriptor().rid())
                && value.nodeGeneration()
                    == snapshot.allocation()
                        .descriptorLifecycleGeneration();
            })
            .map(value -> {
                var route =
                    new ZLinkInternalMeshNode.SpotAuthorityRoute(
                        value.spotId(),
                        snapshot.objectGeneration(),
                        value.nodeRid(),
                        value.nodeGeneration(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerLeaseGeneration(),
                        snapshot.ownerId(),
                        value.meshName(),
                        snapshot.storeVersion());
                var instance =
                    new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                        value.nodeRid(),
                        value.nodeGeneration(),
                        value.spotId(),
                        snapshot.objectGeneration(),
                        snapshot.ownerId(),
                        snapshot.authorityOwnerGeneration(),
                        snapshot.ownerLeaseGeneration(),
                        snapshot.storeVersion());
                boolean ready = snapshot.allocation().state()
                    == ZLinkPlacementAllocationState.ACTIVE;
                return value.instance()
                    .<Applied>map(ignored -> new InstanceApplied(
                        value.stableType(), value.meshName(), ready, route, instance,
                        value.activationRecoveryState()))
                    .orElseGet(() -> new UserApplied(
                        value.stableType(), value.meshName(), ready, route));
            });
    }

    private CompletionStage<Map<String, Applied>> recoverActivations(
        Map<String, Applied> routes) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (Map.Entry<String, Applied> entry : routes.entrySet()) {
            if (!(entry.getValue() instanceof InstanceApplied instance)
                || instance.activationRecovery().isEmpty()) {
                continue;
            }
            ZLinkInternalMeshNode node = meshNodes.get(instance.meshName());
            if (node == null
                || !instance.instance().targetNodeRid().equals(node.routingId())
                || instance.instance().targetNodeGeneration()
                    != node.lifecycleGeneration()) {
                continue;
            }
            tail = tail.thenCompose(ignored -> recoverActivation(
                    entry.getKey(), node, instance)
                .thenAccept(recovered -> routes.put(entry.getKey(), recovered)));
        }
        return tail.thenApply(ignored -> routes);
    }

    private CompletionStage<InstanceApplied> recoverActivation(
        String key,
        ZLinkInternalMeshNode node,
        InstanceApplied applied) {
        ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState recovery =
            applied.activationRecovery().orElseThrow();
        if (relocationStore == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "Instance activation recovery requires a Relocation Store"));
        }
        CompletionStage<String> completedVersion;
        if (Long.compareUnsigned(
                recovery.replayCursor(), recovery.inboxSequence()) < 0) {
            completedVersion = relocationStore.get(recovery.reference(), OPEN)
                .thenCompose(read -> {
                    if (!(read instanceof ZLinkRelocationFound found)) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "Instance activation recovery root is missing"));
                    }
                    byte[] payload = found.payload();
                    if (payload.length != recovery.encodedSize()
                        || !Arrays.equals(sha256(payload), recovery.sha256())) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "Instance activation recovery root failed integrity validation"));
                    }
                    var envelope = new systems.zlink.framework.runtime.internal.service
                        .ZLinkInstanceActivationRecoveryCodec().decode(payload);
                    if (!envelope.targetSpotId().equals(
                            applied.instance().targetSpotId())
                        || !envelope.stableType().equals(applied.stableType())
                        || !envelope.targetMeshName().equals(applied.meshName())
                        || !envelope.targetNodeRid().equals(
                            applied.instance().targetNodeRid())
                        || envelope.targetNodeGeneration()
                            != applied.instance().targetNodeGeneration()
                        || !envelope.descriptorVersion().equals(
                            Long.toString(node.status().descriptorRevision()))) {
                        return CompletableFuture.failedFuture(
                            new IllegalStateException(
                                "Instance activation recovery root does not match authority"));
                    }
                    return node.recoverInstanceActivation(
                            envelope, applied.instance())
                        .thenCompose(ignored -> store.compareExchange(
                            key,
                            new ZLinkAuthorityExpectFound(
                                applied.instance().storeVersion()),
                            new ZLinkAuthorityPut(encodeReady(
                                applied,
                                Optional.of(new ZLinkServiceAuthorityPayloadCodec
                                    .ActivationRecoveryState(
                                        recovery.reference(), recovery.sha256(),
                                        recovery.encodedSize(), recovery.inboxSequence(),
                                        recovery.inboxSequence())))),
                            OPEN))
                        .thenApply(result -> requireStored(
                            result,
                            "Instance activation terminal completion record")
                            .storeVersion());
                });
        } else {
            completedVersion = CompletableFuture.completedFuture(
                applied.instance().storeVersion());
        }
        return completedVersion.thenCompose(storeVersion -> store.compareExchange(
                key,
                new ZLinkAuthorityExpectFound(storeVersion),
                new ZLinkAuthorityPut(encodeReady(applied, Optional.empty())),
                OPEN))
            .thenCompose(result -> {
                ZLinkAuthorityStored released = requireStored(
                    result, "Instance activation recovery pointer release");
                return relocationStore.delete(recovery.reference(), OPEN)
                    .handle((ignored, failure) -> {
                        if (failure != null) {
                            reportFailure.accept(unwrap(failure));
                        }
                        return withoutRecovery(applied, released);
                    });
            });
    }

    private byte[] encodeReady(
        InstanceApplied applied,
        Optional<ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState> recovery) {
        return payloadCodec.encodeInstance(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            applied.stableType(),
            applied.instance().targetSpotId(),
            applied.route().ownerId(),
            applied.instance().leaseGeneration(),
            applied.meshName(),
            applied.instance().targetNodeRid(),
            applied.instance().targetNodeGeneration(),
            recovery);
    }

    private static ZLinkAuthorityStored requireStored(
        systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult result,
        String operation) {
        if (result instanceof ZLinkAuthorityStored stored) return stored;
        throw new IllegalStateException(
            operation + " failed: " + result.getClass().getSimpleName());
    }

    private static InstanceApplied withoutRecovery(
        InstanceApplied applied,
        ZLinkAuthorityStored stored) {
        var route = applied.route();
        var instance = applied.instance();
        return new InstanceApplied(
            applied.stableType(), applied.meshName(), true,
            new ZLinkInternalMeshNode.SpotAuthorityRoute(
                route.spotId(), route.objectGeneration(), route.targetNodeRid(),
                route.targetNodeGeneration(), route.authorityOwnerGeneration(),
                route.ownerLeaseGeneration(), route.ownerId(), route.meshName(),
                stored.storeVersion()),
            new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                instance.targetNodeRid(), instance.targetNodeGeneration(),
                instance.targetSpotId(), instance.objectGeneration(),
                instance.ownerId(), instance.authorityOwnerGeneration(),
                instance.leaseGeneration(), stored.storeVersion()),
            Optional.empty());
    }

    private static byte[] sha256(byte[] payload) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(payload);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private void applyCore(Map<String, Applied> next) {
        for (Map.Entry<String, Applied> old : applied.entrySet()) {
            Applied current = next.get(old.getKey());
            if (!old.getValue().equals(current)) {
                forget(old.getValue());
            }
        }
        for (Map.Entry<String, Applied> entry : next.entrySet()) {
            if (!entry.getValue().equals(applied.get(entry.getKey()))) {
                remember(entry.getValue());
            }
        }
        applied.clear();
        applied.putAll(next);
    }

    private void remember(Applied value) {
        ZLinkInternalMeshNode node =
            meshNodes.get(value.meshName());
        if (node == null) {
            return;
        }
        if (value.ready()) {
            node.rememberSpotAuthority(value.route());
        }
        if (value instanceof InstanceApplied instance) {
            node.registerInstanceIntent(
                value.stableType(), instance.instance());
        }
    }

    private void forget(Applied value) {
        ZLinkInternalMeshNode node =
            meshNodes.get(value.meshName());
        if (node == null) {
            return;
        }
        if (value.ready()) {
            node.forgetSpotAuthority(value.route());
        }
        if (value instanceof InstanceApplied instance) {
            node.forgetInstanceIntent(instance.instance());
        }
    }

    private void poll() {
        if (closed || !started
            || !inFlight.compareAndSet(false, true)) {
            return;
        }
        reconcile().whenComplete((ignored, failure) -> {
            inFlight.set(false);
            if (failure != null && !closed) {
                reportFailure.accept(unwrap(failure));
            }
        });
    }

    @Override
    public void close() {
        inStateLane(() -> {
            closed = true;
            executor.shutdownNow();
            applied.values().forEach(this::forget);
            applied.clear();
            return null;
        });
    }

    private <T> T inStateLane(java.util.function.Supplier<T> work) {
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

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException
                && failure.getCause() != null
            ? failure.getCause()
            : failure;
    }

    private sealed interface Applied permits UserApplied, InstanceApplied {
        String stableType();
        String meshName();
        boolean ready();
        ZLinkInternalMeshNode.SpotAuthorityRoute route();
    }

    private record UserApplied(
        String stableType,
        String meshName,
        boolean ready,
        ZLinkInternalMeshNode.SpotAuthorityRoute route) implements Applied {
    }

    private record InstanceApplied(
        String stableType,
        String meshName,
        boolean ready,
        ZLinkInternalMeshNode.SpotAuthorityRoute route,
        ZLinkServiceM6BWireCodec.InstanceRouteFence instance,
        Optional<ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState>
            activationRecovery) implements Applied {
    }

}
