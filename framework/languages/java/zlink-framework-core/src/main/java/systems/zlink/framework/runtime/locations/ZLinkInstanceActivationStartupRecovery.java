package systems.zlink.framework.runtime.locations;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanCursor;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanExpired;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;

/** Restores durable Instance activation work once, before host admission begins. */
public final class ZLinkInstanceActivationStartupRecovery {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    private final ZLinkLocationRepository store;
    private final ZLinkRelocationStore relocationStore;
    private final Map<String, ZLinkInternalMeshNode> meshNodes;
    private final ZLinkServiceAuthorityPayloadCodec payloadCodec =
            new ZLinkServiceAuthorityPayloadCodec();

    public ZLinkInstanceActivationStartupRecovery(
            ZLinkLocationRepository store,
            ZLinkRelocationStore relocationStore,
            Map<String, ZLinkInternalMeshNode> meshNodes) {
        this.store = Objects.requireNonNull(store, "store");
        this.relocationStore = relocationStore;
        this.meshNodes = Map.copyOf(Objects.requireNonNull(meshNodes, "meshNodes"));
    }

    public CompletionStage<Void> start() {
        return scan(Optional.empty(), new HashMap<>()).thenCompose(this::recoverActivations);
    }

    private CompletionStage<Map<String, InstanceRecovery>> scan(
            Optional<ZLinkAuthorityScanCursor> cursor, Map<String, InstanceRecovery> routes) {
        return store.list(ZLinkAuthorityKeyCodec.spotPrefix(), cursor, 1000, OPEN)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkAuthorityScanExpired) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException("authority scan expired"));
                            }
                            ZLinkAuthorityPage page = (ZLinkAuthorityPage) result;
                            for (var entry : page.items()) {
                                decode(entry.snapshot())
                                        .filter(value -> value.activationRecovery().isPresent())
                                        .ifPresent(value -> routes.put(entry.key(), value));
                            }
                            return page.nextCursor().isEmpty()
                                    ? CompletableFuture.completedFuture(routes)
                                    : scan(page.nextCursor(), routes);
                        });
    }

    private Optional<InstanceRecovery> decode(ZLinkAuthoritySnapshot snapshot) {
        if (snapshot.allocation().objectKind() != ZLinkPlacementObjectKind.INSTANCE_SPOT) {
            return Optional.empty();
        }
        return payloadCodec
                .decode(snapshot.payload())
                .filter(
                        value -> {
                            boolean ready =
                                    snapshot.allocation().state()
                                                    == ZLinkPlacementAllocationState.ACTIVE
                                            && value.state()
                                                    == ZLinkServiceAuthorityPayloadCodec.State
                                                            .READY;
                            boolean coldActivation =
                                    snapshot.allocation().state()
                                                    == ZLinkPlacementAllocationState.PENDING
                                            && value.instance().isPresent()
                                            && value.state()
                                                    == ZLinkServiceAuthorityPayloadCodec.State
                                                            .CREATING;
                            return value.instance().isPresent()
                                    && (ready || coldActivation)
                                    && value.ownerId().equals(snapshot.ownerId())
                                    && value.ownerLeaseGeneration()
                                            == snapshot.ownerLeaseGeneration()
                                    && value.meshName()
                                            .equals(snapshot.allocation().descriptor().meshName())
                                    && value.nodeRid()
                                            .equals(snapshot.allocation().descriptor().rid())
                                    && value.nodeGeneration()
                                            == snapshot.allocation()
                                                    .descriptorLifecycleGeneration();
                        })
                .map(
                        value -> {
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
                            boolean ready =
                                    snapshot.allocation().state()
                                            == ZLinkPlacementAllocationState.ACTIVE;
                            return new InstanceRecovery(
                                    value.stableType(),
                                    value.meshName(),
                                    ready,
                                    instance,
                                    value.activationRecoveryState()
                                            .or(
                                                    () ->
                                                            snapshot.pendingCreation()
                                                                    .map(
                                                                            pending ->
                                                                                    new ZLinkServiceAuthorityPayloadCodec
                                                                                            .ActivationRecoveryState(
                                                                                            pending
                                                                                                    .requestContentReference(),
                                                                                            pending
                                                                                                    .requestSha256(),
                                                                                            pending
                                                                                                    .requestEncodedSize(),
                                                                                            1,
                                                                                            0))));
                        });
    }

    private CompletionStage<Void> recoverActivations(Map<String, InstanceRecovery> routes) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (Map.Entry<String, InstanceRecovery> entry : routes.entrySet()) {
            InstanceRecovery instance = entry.getValue();
            ZLinkInternalMeshNode node = meshNodes.get(instance.meshName());
            if (node == null) continue;
            if (!instance.instance().targetNodeRid().equals(node.routingId())
                    || instance.instance().targetNodeGeneration() != node.lifecycleGeneration()) {
                if (!instance.ready()) {
                    tail =
                            tail.thenCompose(
                                    ignored ->
                                            store.releaseEndedReservation(
                                                            entry.getKey(),
                                                            instance.instance().storeVersion(),
                                                            OPEN)
                                                    .thenCompose(
                                                            released ->
                                                                    released
                                                                            ? relocationStore
                                                                                    .delete(
                                                                                            instance.activationRecovery()
                                                                                                    .orElseThrow()
                                                                                                    .reference(),
                                                                                            OPEN)
                                                                                    .thenApply(
                                                                                            deleted ->
                                                                                                    null)
                                                                            : CompletableFuture
                                                                                    .completedFuture(
                                                                                            null)));
                }
                continue;
            }
            tail = tail.thenCompose(ignored -> recoverActivation(entry.getKey(), node, instance));
        }
        return tail;
    }

    private CompletionStage<Void> recoverActivation(
            String key, ZLinkInternalMeshNode node, InstanceRecovery applied) {
        ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState recovery =
                applied.activationRecovery().orElseThrow();
        if (relocationStore == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Instance activation recovery requires a Relocation Store"));
        }
        CompletionStage<Void> replay =
                Long.compareUnsigned(recovery.replayCursor(), recovery.inboxSequence()) < 0
                        ? relocationStore
                                .get(recovery.reference(), OPEN)
                                .thenCompose(
                                        read -> {
                                            if (!(read instanceof ZLinkRelocationFound found))
                                                return CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "Instance activation recovery root"
                                                                        + " is missing"));
                                            byte[] payload = found.payload();
                                            if (payload.length != recovery.encodedSize()
                                                    || !Arrays.equals(
                                                            sha256(payload), recovery.sha256())) {
                                                return CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "Instance activation recovery root"
                                                                        + " failed integrity"
                                                                        + " validation"));
                                            }
                                            var envelope =
                                                    new systems.zlink.framework.runtime.internal
                                                                    .service
                                                                    .ZLinkInstanceActivationRecoveryCodec()
                                                            .decode(payload);
                                            if (!envelope.targetSpotId()
                                                            .equals(
                                                                    applied.instance()
                                                                            .targetSpotId())
                                                    || !envelope.stableType()
                                                            .equals(applied.stableType())
                                                    || !envelope.targetMeshName()
                                                            .equals(applied.meshName())
                                                    || !envelope.targetNodeRid()
                                                            .equals(
                                                                    applied.instance()
                                                                            .targetNodeRid())
                                                    || envelope.targetNodeGeneration()
                                                            != applied.instance()
                                                                    .targetNodeGeneration()) {
                                                return CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "Instance activation recovery root"
                                                                        + " does not match authority"));
                                            }
                                            return node.recoverInstanceActivation(
                                                    envelope, applied.instance());
                                        })
                        : CompletableFuture.completedFuture(null);
        return replay.thenCompose(ignored -> store.read(key, OPEN))
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkAuthoritySnapshot current))
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "Recovered Instance authority is missing"));
                            InstanceRecovery observed = decode(current).orElse(null);
                            if (observed == null
                                    || !observed.ready()
                                    || !observed.stableType().equals(applied.stableType())
                                    || !observed.meshName().equals(applied.meshName())
                                    || !observed.instance()
                                            .equals(
                                                    withStoreVersion(
                                                            applied.instance(),
                                                            current.storeVersion()))) {
                                return CompletableFuture.failedFuture(
                                        new IllegalStateException(
                                                "Instance activation recovery owner changed"));
                            }
                            if (observed.activationRecovery().isEmpty()) {
                                return CompletableFuture.completedFuture(null);
                            }
                            return completeActivationRecovery(store, relocationStore, key, current)
                                    .handle(
                                            (released, failure) -> {
                                                Throwable recoveryFailure = unwrap(failure);
                                                return node.instanceActivationRecoveryCompleted(
                                                                applied.instance(), recoveryFailure)
                                                        .handle(
                                                                (ignored, notificationFailure) -> {
                                                                    if (recoveryFailure != null) {
                                                                        if (notificationFailure
                                                                                        != null
                                                                                && notificationFailure
                                                                                        != recoveryFailure)
                                                                            recoveryFailure
                                                                                    .addSuppressed(
                                                                                            notificationFailure);
                                                                        throw new java.util
                                                                                .concurrent
                                                                                .CompletionException(
                                                                                recoveryFailure);
                                                                    }
                                                                    if (notificationFailure != null)
                                                                        throw new java.util
                                                                                .concurrent
                                                                                .CompletionException(
                                                                                notificationFailure);
                                                                    return (Void) null;
                                                                });
                                            })
                                    .thenCompose(java.util.function.Function.identity());
                        });
    }

    /** Records handler completion before releasing its immutable activation root. */
    public static CompletionStage<ZLinkAuthorityStored> completeActivationRecovery(
            ZLinkLocationRepository store,
            ZLinkRelocationStore relocationStore,
            String key,
            ZLinkAuthoritySnapshot snapshot) {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var authority = codec.decode(snapshot.payload()).orElseThrow();
        var recovery = authority.activationRecoveryState().orElseThrow();
        java.util.function.Function<
                        Optional<ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState>, byte[]>
                encode =
                        pointer ->
                                codec.encodeInstance(
                                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                                        authority.stableType(),
                                        authority.spotId(),
                                        snapshot.ownerId(),
                                        snapshot.ownerLeaseGeneration(),
                                        authority.meshName(),
                                        authority.nodeRid(),
                                        authority.nodeGeneration(),
                                        pointer);
        CompletionStage<String> cursor =
                Long.compareUnsigned(recovery.replayCursor(), recovery.inboxSequence()) < 0
                        ? store.compareExchange(
                                        key,
                                        new ZLinkAuthorityExpectFound(snapshot.storeVersion()),
                                        new ZLinkAuthorityPut(
                                                encode.apply(
                                                        Optional.of(
                                                                new ZLinkServiceAuthorityPayloadCodec
                                                                        .ActivationRecoveryState(
                                                                        recovery.reference(),
                                                                        recovery.sha256(),
                                                                        recovery.encodedSize(),
                                                                        recovery.inboxSequence(),
                                                                        recovery
                                                                                .inboxSequence())))),
                                        OPEN)
                                .thenApply(
                                        result ->
                                                requireStored(
                                                                result,
                                                                "Instance activation handler"
                                                                        + " completion record")
                                                        .storeVersion())
                        : CompletableFuture.completedFuture(snapshot.storeVersion());
        return cursor.thenCompose(
                        version ->
                                store.compareExchange(
                                        key,
                                        new ZLinkAuthorityExpectFound(version),
                                        new ZLinkAuthorityPut(encode.apply(Optional.empty())),
                                        OPEN))
                .thenCompose(
                        result -> {
                            var released =
                                    requireStored(
                                            result, "Instance activation recovery pointer release");
                            return relocationStore
                                    .delete(recovery.reference(), OPEN)
                                    .thenApply(ignored -> released);
                        });
    }

    private static ZLinkAuthorityStored requireStored(
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityWriteResult result,
            String operation) {
        if (result instanceof ZLinkAuthorityStored stored) return stored;
        throw new IllegalStateException(
                operation + " failed: " + result.getClass().getSimpleName());
    }

    private static ZLinkServiceM6BWireCodec.InstanceRouteFence withStoreVersion(
            ZLinkServiceM6BWireCodec.InstanceRouteFence instance, String storeVersion) {
        return new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                instance.targetNodeRid(), instance.targetNodeGeneration(),
                instance.targetSpotId(), instance.objectGeneration(),
                instance.ownerId(), instance.authorityOwnerGeneration(),
                instance.leaseGeneration(), storeVersion);
    }

    private static byte[] sha256(byte[] payload) {
        try {
            return MessageDigest.getInstance("SHA-256").digest(payload);
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException(impossible);
        }
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException && failure.getCause() != null
                ? failure.getCause()
                : failure;
    }

    private record InstanceRecovery(
            String stableType,
            String meshName,
            boolean ready,
            ZLinkServiceM6BWireCodec.InstanceRouteFence instance,
            Optional<ZLinkServiceAuthorityPayloadCodec.ActivationRecoveryState>
                    activationRecovery) {}
}
