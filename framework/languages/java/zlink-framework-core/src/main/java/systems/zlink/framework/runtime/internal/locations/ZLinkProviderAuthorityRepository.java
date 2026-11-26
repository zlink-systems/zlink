package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.HashMap;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ForkJoinPool;

/** Implements Framework-owned authority records over the opaque provider SPI. */
final class ZLinkProviderAuthorityRepository {
    private static final int AUTHORITY_RECORD_VERSION = 1;
    private static final String FIELD_RECORD_VERSION = "recordVersion";
    private static final String OWNER_TRANSITION_PRESERVE = "preserve";
    private static final String OWNER_TRANSITION_NEW_OWNER = "newOwner";
    private static final ObjectMapper CANONICAL_JSON = new ObjectMapper();
    private static final String AGGREGATE_PREFIX = "zlink:v11:aggregate:";
    private static final byte AGGREGATE_STAGING = 0;
    private static final byte AGGREGATE_PREPARED = 1;
    private static final byte AGGREGATE_COMMITTED = 2;
    private static final Duration AGGREGATE_PREPARE_RETRY_WINDOW = Duration.ofSeconds(5);
    private final ZLinkLocationStore provider;
    private final java.util.concurrent.ConcurrentHashMap<
                    ZLinkMeshNodeDescriptorKey, CapacityMetricSource>
            capacityMetrics = new java.util.concurrent.ConcurrentHashMap<>();

    private volatile boolean capacityObservationRegistered;

    ZLinkRuntimeMetrics.Registration registerCapacityObservation() {
        capacityObservationRegistered = true;
        return () -> {
            capacityObservationRegistered = false;
            capacityMetrics.keySet().forEach(this::removeCapacityMetrics);
            capacityMetrics.clear();
        };
    }

    void registerCapacityMetrics(ZLinkMeshNodeDescriptor descriptor) {
        if (!capacityObservationRegistered) return;
        var key = new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid());
        capacityMetrics.compute(
                key,
                (ignored, source) -> {
                    if (source == null) return new CapacityMetricSource(descriptor);
                    source.descriptor = descriptor;
                    return source;
                });
    }

    CompletionStage<Void> refreshCapacityMetrics(ZLinkMeshNodeDescriptorKey descriptor) {
        if (!capacityMetrics.containsKey(descriptor)) return completed(null);
        return readCapacityProjection(descriptor).thenAccept(ignored -> {});
    }

    CompletionStage<ZLinkMeshNodeDescriptor> projectCapacity(ZLinkMeshNodeDescriptor descriptor) {
        var key = new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid());
        return readCapacityProjection(key)
                .thenApply(
                        counts -> {
                            var configured = descriptor.capacity();
                            var projected =
                                    new ZLinkPlacementCapacity(
                                            capacityUsage(
                                                    key,
                                                    counts,
                                                    "actors",
                                                    configured.actors().limit()),
                                            capacityUsage(
                                                    key,
                                                    counts,
                                                    "spots",
                                                    configured.spots().limit()),
                                            configured.spotTypes().stream()
                                                    .map(
                                                            type ->
                                                                    new ZLinkSpotTypeCapacity(
                                                                            type.objectKind(),
                                                                            type.stableType(),
                                                                            capacityUsage(
                                                                                    key,
                                                                                    counts,
                                                                                    ZLinkCapacityCounters
                                                                                            .spotType(
                                                                                                    type
                                                                                                            .objectKind(),
                                                                                                    type
                                                                                                            .stableType()),
                                                                                    type.usage()
                                                                                            .limit())))
                                                    .toList());
                            return new ZLinkMeshNodeDescriptor(
                                    descriptor.meshName(),
                                    descriptor.rid(),
                                    descriptor.lifecycleGeneration(),
                                    descriptor.descriptorRevision(),
                                    descriptor.endpoint(),
                                    descriptor.channelWeights(),
                                    descriptor.applicationVersion(),
                                    descriptor.objectCapabilities(),
                                    descriptor.objectRole(),
                                    descriptor.entrySpotId(),
                                    descriptor.placementWeight(),
                                    projected,
                                    descriptor.activationConcurrency(),
                                    descriptor.maintenanceWave(),
                                    descriptor.state(),
                                    descriptor.securityIdentity(),
                                    descriptor.ownerId(),
                                    descriptor.leaseGeneration(),
                                    descriptor.updatedAt());
                        });
    }

    private static ZLinkCapacityUsage capacityUsage(
            ZLinkMeshNodeDescriptorKey key,
            Map<ZLinkStoreKey, Long> counts,
            String dimension,
            int limit) {
        return new ZLinkCapacityUsage(
                Math.toIntExact(
                        counts.getOrDefault(
                                ZLinkCapacityCounters.key(key, dimension, "active"), 0L)),
                Math.toIntExact(
                        counts.getOrDefault(
                                ZLinkCapacityCounters.key(key, dimension, "pending"), 0L)),
                limit);
    }

    private CompletionStage<Map<ZLinkStoreKey, Long>> readCapacityProjection(
            ZLinkMeshNodeDescriptorKey key) {
        return readCapacityPage(ZLinkCapacityCounters.prefix(key), null, new HashMap<>())
                .thenApply(
                        counts -> {
                            capacityMetrics.computeIfPresent(
                                    key,
                                    (ignored, source) -> {
                                        boolean firstConfirmation = source.current == null;
                                        source.current = counts;
                                        if (firstConfirmation) source.register(key);
                                        return source;
                                    });
                            return counts;
                        });
    }

    private CompletionStage<Map<ZLinkStoreKey, Long>> readCapacityPage(
            String prefix, ZLinkStoreScanCursor cursor, Map<ZLinkStoreKey, Long> counts) {
        return provider.scan(new ZLinkStoreScanRequest(prefix, cursor, 1_000), () -> false)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                return CompletableFuture.completedFuture(null)
                                        .thenComposeAsync(
                                                ignored ->
                                                        readCapacityPage(
                                                                prefix, null, new HashMap<>()));
                            }
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            page.items()
                                    .forEach(
                                            item ->
                                                    counts.put(
                                                            item.key(),
                                                            decodeCapacityCount(
                                                                    item.value().bytes())));
                            return page.nextCursor() == null
                                    ? completed(counts)
                                    : readCapacityPage(prefix, page.nextCursor(), counts);
                        });
    }

    private static long decodeCapacityCount(byte[] bytes) {
        try {
            return ZLinkLocationStoreFormat.decodeCounter(bytes);
        } catch (NumberFormatException failure) {
            throw new IllegalStateException("Location Store capacity counter is invalid", failure);
        }
    }

    void removeCapacityMetrics(ZLinkMeshNodeDescriptorKey descriptor) {
        capacityMetrics.computeIfPresent(
                descriptor,
                (ignored, source) -> {
                    source.close();
                    return null;
                });
    }

    private final class CapacityMetricSource {
        private volatile ZLinkMeshNodeDescriptor descriptor;
        private volatile Map<ZLinkStoreKey, Long> current;
        private final List<ZLinkRuntimeMetrics.Registration> registrations = new ArrayList<>();

        CapacityMetricSource(ZLinkMeshNodeDescriptor descriptor) {
            this.descriptor = descriptor;
        }

        private void register(ZLinkMeshNodeDescriptorKey key) {
            var actors =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            descriptor.meshName(),
                            ZLinkRuntimeMetrics.Tag.CAPACITY_SCOPE.wire(),
                            "actor");
            var spots =
                    Map.of(
                            ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                            descriptor.meshName(),
                            ZLinkRuntimeMetrics.Tag.CAPACITY_SCOPE.wire(),
                            "spot");
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_ACTIVE,
                    actors,
                    counter(key, "actors", "active"));
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_RESERVED,
                    actors,
                    counter(key, "actors", "pending"));
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_LIMIT,
                    actors,
                    () -> descriptor.capacity().actors().limit());
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_ACTIVE,
                    spots,
                    counter(key, "spots", "active"));
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_RESERVED,
                    spots,
                    counter(key, "spots", "pending"));
            observe(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                            .OBJECT_CAPACITY_LIMIT,
                    spots,
                    () -> descriptor.capacity().spots().limit());
            for (var type : descriptor.capacity().spotTypes()) {
                String kind =
                        type.objectKind() == ZLinkPlacementObjectKind.INSTANCE_SPOT
                                ? ZLinkRuntimeMetrics.SpotKind.INSTANCE.wire()
                                : ZLinkRuntimeMetrics.SpotKind.USER.wire();
                String dimension =
                        ZLinkCapacityCounters.spotType(type.objectKind(), type.stableType());
                var tags =
                        Map.of(
                                ZLinkRuntimeMetrics.Tag.MESH_NAME.wire(),
                                descriptor.meshName(),
                                ZLinkRuntimeMetrics.Tag.SPOT_KIND.wire(),
                                kind,
                                ZLinkRuntimeMetrics.Tag.STABLE_TYPE.wire(),
                                type.stableType());
                observe(
                        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                                .SPOT_TYPE_CAPACITY_ACTIVE,
                        tags,
                        counter(key, dimension, "active"));
                observe(
                        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                                .SPOT_TYPE_CAPACITY_RESERVED,
                        tags,
                        counter(key, dimension, "pending"));
                observe(
                        systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric
                                .SPOT_TYPE_CAPACITY_LIMIT,
                        tags,
                        () -> type.usage().limit());
            }
        }

        private java.util.function.LongSupplier counter(
                ZLinkMeshNodeDescriptorKey descriptorKey, String dimension, String state) {
            ZLinkStoreKey key = ZLinkCapacityCounters.key(descriptorKey, dimension, state);
            return () -> current.getOrDefault(key, 0L);
        }

        private void observe(
                systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics.Metric metric,
                Map<String, String> tags,
                java.util.function.LongSupplier value) {
            registrations.add(
                    systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics
                            .registerObservable(metric, tags, value));
        }

        private void close() {
            registrations.forEach(ZLinkRuntimeMetrics.Registration::close);
            registrations.clear();
        }
    }

    private final ZLinkProviderDescriptorRepository descriptors;
    private final ZLinkAggregateInventoryStore aggregateInventory;
    private final ZLinkOwnerLeaseWaitTracker ownerLeaseWait;

    ZLinkProviderAuthorityRepository(ZLinkLocationStore provider) {
        this(
                provider,
                new ZLinkProviderDescriptorRepository(provider),
                new ZLinkOwnerLeaseWaitTracker(System::nanoTime, Duration.ZERO));
    }

    ZLinkProviderAuthorityRepository(
            ZLinkLocationStore provider, ZLinkProviderDescriptorRepository descriptors) {
        this(
                provider,
                descriptors,
                new ZLinkOwnerLeaseWaitTracker(System::nanoTime, Duration.ZERO));
    }

    ZLinkProviderAuthorityRepository(
            ZLinkLocationStore provider,
            ZLinkProviderDescriptorRepository descriptors,
            ZLinkOwnerLeaseWaitTracker ownerLeaseWait) {
        this.provider = Objects.requireNonNull(provider, "provider");
        this.descriptors = Objects.requireNonNull(descriptors, "descriptors");
        this.aggregateInventory = new ZLinkAggregateInventoryStore(provider);
        this.ownerLeaseWait = Objects.requireNonNull(ownerLeaseWait, "ownerLeaseWait");
    }

    CompletionStage<ZLinkAuthorityReadResult> read(
            String key, ZLinkStoreCancellation cancellation) {
        requireKey(key);
        ZLinkStoreKey rowKey = authorityKey(key);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(rowKey, opaqueCancellation)
                .thenCompose(result -> projectRead(result, opaqueCancellation));
    }

    CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
            String key,
            ZLinkAuthorityExpectation expectation,
            ZLinkAuthorityMutation mutation,
            ZLinkStoreCancellation cancellation) {
        requireKey(key);
        Objects.requireNonNull(expectation, "expectation");
        Objects.requireNonNull(mutation, "mutation");
        var opaqueCancellation = adapt(cancellation);
        ZLinkStoreKey rowKey = authorityKey(key);
        return provider.read(rowKey, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return projectRead(read, opaqueCancellation)
                                        .thenApply(visible -> new ZLinkAuthorityConflict(visible));
                            }
                            AuthorityRecord current = decode(found.value().bytes());
                            if (current.aggregate() != null
                                    || (!(mutation instanceof ZLinkAuthorityRestore)
                                            && current.allocation().state()
                                                    != ZLinkPlacementAllocationState.ACTIVE)) {
                                return projectRead(read, opaqueCancellation)
                                        .thenApply(
                                                visible ->
                                                        new ZLinkAuthorityConflict(
                                                                visible,
                                                                ZLinkFrameworkErrorKind
                                                                        .UNAVAILABLE));
                            }
                            if (!(expectation instanceof ZLinkAuthorityExpectFound expected)
                                    || !found.value()
                                            .version()
                                            .value()
                                            .equals(expected.storeVersion())) {
                                return projectRead(read, opaqueCancellation)
                                        .thenApply(visible -> new ZLinkAuthorityConflict(visible));
                            }
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            conditions.add(
                                    new ZLinkStoreVersionCondition(
                                            rowKey, found.value().version()));
                            if (mutation instanceof ZLinkAuthorityDelete) {
                                var descriptor = current.allocation().descriptor();
                                var descriptorKey =
                                        ZLinkProviderDescriptorRepository.meshKey(
                                                descriptor.meshName(), descriptor.rid());
                                return provider.read(descriptorKey, opaqueCancellation)
                                        .thenCompose(
                                                descriptorRead -> {
                                                    conditions.add(
                                                            descriptorRead
                                                                            instanceof
                                                                            ZLinkStoreReadFound
                                                                                    descriptorFound
                                                                    ? new ZLinkStoreVersionCondition(
                                                                            descriptorKey,
                                                                            descriptorFound
                                                                                    .value()
                                                                                    .version())
                                                                    : new ZLinkStoreMissingCondition(
                                                                            descriptorKey));
                                                    return requireLiveOwner(
                                                            current,
                                                            conditions,
                                                            opaqueCancellation);
                                                })
                                        .thenCompose(
                                                live -> {
                                                    if (!live) {
                                                        return completed(
                                                                new ZLinkAuthorityConflict(
                                                                        toRead(read)));
                                                    }
                                                    var deltas = new ZLinkCapacityCounters();
                                                    var allocation = current.allocation();
                                                    deltas.change(
                                                            allocation.descriptor(),
                                                            allocation.capacityBundle(),
                                                            0,
                                                            -1);
                                                    List<ZLinkStoreMutation> mutations =
                                                            new ArrayList<>();
                                                    mutations.add(new ZLinkStoreDelete(rowKey));
                                                    deltas.appendTo(mutations);
                                                    return writeAuthority(
                                                            rowKey,
                                                            found,
                                                            null,
                                                            conditions,
                                                            mutations,
                                                            cancellation,
                                                            key,
                                                            mutation);
                                                });
                            }
                            if (mutation instanceof ZLinkAuthorityRestore restore) {
                                if (!current.ownerId().equals(restore.expectedOwner().ownerId())
                                        || current.ownerLeaseGeneration()
                                                != restore.expectedOwner().leaseGeneration()) {
                                    return completed(new ZLinkAuthorityConflict(toRead(read)));
                                }
                                AuthorityRecord next = current.withPayload(restore.payload());
                                return put(
                                        rowKey,
                                        found,
                                        next,
                                        conditions,
                                        cancellation,
                                        key,
                                        mutation);
                            }
                            if (mutation instanceof ZLinkAuthorityPut preserve
                                    && preserve.preserved() != null) {
                                if (!preserve.matchesPreserved(snapshot(found.value()))
                                        || !snapshot(found.value()).steadyReady()
                                        || !preserve.replacementOwner()
                                                .ownerId()
                                                .equals(current.ownerId())
                                        || preserve.replacementOwner().leaseGeneration()
                                                == current.ownerLeaseGeneration())
                                    return completed(new ZLinkAuthorityConflict(toRead(read)));
                                return descriptors
                                        .readMeshNode(
                                                current.allocation().descriptor(),
                                                conditions,
                                                opaqueCancellation)
                                        .thenCompose(
                                                descriptor -> {
                                                    if (descriptor.isEmpty()
                                                            || descriptor
                                                                            .get()
                                                                            .lifecycleGeneration()
                                                                    != current.allocation()
                                                                            .descriptorLifecycleGeneration()
                                                            || !descriptor
                                                                    .get()
                                                                    .ownerId()
                                                                    .equals(current.ownerId()))
                                                        return completed(
                                                                new ZLinkAuthorityConflict(
                                                                        toRead(read)));
                                                    return requireLiveOwner(
                                                                    preserve.replacementOwner(),
                                                                    conditions,
                                                                    opaqueCancellation)
                                                            .thenCompose(
                                                                    live ->
                                                                            !live
                                                                                    ? completed(
                                                                                            new ZLinkAuthorityConflict(
                                                                                                    toRead(
                                                                                                            read)))
                                                                                    : put(
                                                                                            rowKey,
                                                                                            found,
                                                                                            current
                                                                                                    .withOwner(
                                                                                                            preserve
                                                                                                                    .payload(),
                                                                                                            current
                                                                                                                    .authorityOwnerGeneration(),
                                                                                                            preserve
                                                                                                                    .replacementOwner()),
                                                                                            conditions,
                                                                                            cancellation,
                                                                                            key,
                                                                                            mutation));
                                                });
                            }
                            byte[] reincarnatePayload =
                                    ZLinkAuthorityMutation.reincarnatePayload(mutation);
                            ZLinkLocationOwnerToken owner =
                                    new ZLinkLocationOwnerToken(
                                            current.ownerId(), current.ownerLeaseGeneration());
                            return requireLiveOwner(owner, conditions, opaqueCancellation)
                                    .thenCompose(
                                            live -> {
                                                if (!live) {
                                                    return completed(
                                                            new ZLinkAuthorityConflict(
                                                                    toRead(read),
                                                                    ZLinkFrameworkErrorKind
                                                                            .UNAVAILABLE));
                                                }
                                                if (reincarnatePayload != null) {
                                                    return nextPair(
                                                                    key,
                                                                    conditions,
                                                                    opaqueCancellation)
                                                            .thenCompose(
                                                                    counters -> {
                                                                        if (counters.exhausted()) {
                                                                            return completed(
                                                                                    new ZLinkAuthorityGenerationExhausted());
                                                                        }
                                                                        AuthorityRecord next =
                                                                                new AuthorityRecord(
                                                                                        reincarnatePayload,
                                                                                        counters
                                                                                                .objectGeneration(),
                                                                                        counters
                                                                                                .ownerGeneration(),
                                                                                        current
                                                                                                .ownerId(),
                                                                                        current
                                                                                                .ownerLeaseGeneration(),
                                                                                        current
                                                                                                .allocation(),
                                                                                        current
                                                                                                .pendingCreation());
                                                                        List<ZLinkStoreMutation>
                                                                                mutations =
                                                                                        new ArrayList<>(
                                                                                                counters
                                                                                                        .mutations());
                                                                        mutations.add(
                                                                                new ZLinkStorePut(
                                                                                        rowKey,
                                                                                        encode(
                                                                                                next),
                                                                                        null));
                                                                        return writeAuthority(
                                                                                rowKey,
                                                                                found,
                                                                                next,
                                                                                conditions,
                                                                                mutations,
                                                                                cancellation,
                                                                                key,
                                                                                mutation);
                                                                    });
                                                }
                                                return put(
                                                        rowKey,
                                                        found,
                                                        current.withPayload(
                                                                        ((ZLinkAuthorityPut)
                                                                                        mutation)
                                                                                .payload())
                                                                .withRelocation(
                                                                        ((ZLinkAuthorityPut)
                                                                                                mutation)
                                                                                        .replaceRelocation()
                                                                                ? ((ZLinkAuthorityPut)
                                                                                                mutation)
                                                                                        .relocation()
                                                                                : current
                                                                                        .relocation()),
                                                        conditions,
                                                        cancellation,
                                                        key,
                                                        mutation);
                                            });
                        });
    }

    private static boolean descriptorMatchesTarget(
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkMeshNodeDescriptorKey key,
            long lifecycleGeneration,
            ZLinkLocationOwnerToken owner) {
        return descriptor.meshName().equals(key.meshName())
                && descriptor.rid().equals(key.rid())
                && descriptor.lifecycleGeneration() == lifecycleGeneration
                && descriptor.ownerId().equals(owner.ownerId())
                && descriptor.leaseGeneration() == owner.leaseGeneration();
    }

    private static boolean descriptorAdmits(
            ZLinkMeshNodeDescriptor descriptor, ZLinkObjectReservationRequest request) {
        return descriptorMatchesTarget(
                        descriptor,
                        request.targetDescriptor(),
                        request.targetDescriptorLifecycleGeneration(),
                        request.targetOwner())
                && descriptor.state() == ZLinkFrameworkRuntimeState.SERVING
                && descriptor.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                && descriptor.placementWeight() > 0
                && descriptor.objectCapabilities().stream()
                        .anyMatch(
                                capability ->
                                        capability.objectKind() == request.objectKind()
                                                && capability
                                                        .stableType()
                                                        .equals(request.stableType()))
                && bundleMatchesObject(
                        request.capacityBundle(), request.objectKind(), request.stableType());
    }

    private static boolean bundleMatchesObject(
            ZLinkPlacementCapacityBundle bundle,
            ZLinkPlacementObjectKind objectKind,
            String stableType) {
        if (objectKind == ZLinkPlacementObjectKind.ACTOR) {
            return bundle.actorSlots() > 0
                    && bundle.spotSlots() == 0
                    && bundle.spotType().isEmpty();
        }
        return bundle.actorSlots() == 0
                && bundle.spotSlots() > 0
                && bundle.spotType().isPresent()
                && bundle.spotType().orElseThrow().objectKind() == objectKind
                && bundle.spotType().orElseThrow().stableType().equals(stableType);
    }

    CompletionStage<ZLinkAuthorityScanResult> list(
            String prefix,
            Optional<ZLinkAuthorityScanCursor> cursor,
            int limit,
            ZLinkStoreCancellation cancellation) {
        if (limit <= 0) {
            throw new IllegalArgumentException("authority scan limit must be positive");
        }
        ZLinkStoreScanCursor providerCursor =
                cursor.map(value -> new ZLinkStoreScanCursor(value.encoded())).orElse(null);
        return provider.scan(
                        new ZLinkStoreScanRequest(
                                authorityScanPrefix(prefix), providerCursor, limit),
                        adapt(cancellation))
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                return completed(new ZLinkAuthorityScanExpired());
                            }
                            ZLinkStoreScanPage page = ((ZLinkStoreScanPageResult) result).value();
                            List<ZLinkAuthorityEntry> items = new ArrayList<>();
                            CompletionStage<Void> visible = CompletableFuture.completedFuture(null);
                            for (ZLinkStoreScanItem item : page.items()) {
                                String authorityKey = decodeAuthorityKey(item.key());
                                if (!authorityKey.startsWith(prefix)) {
                                    continue;
                                }
                                visible =
                                        visible.thenCompose(
                                                ignored ->
                                                        projectRead(
                                                                        new ZLinkStoreReadFound(
                                                                                item.value()),
                                                                        adapt(cancellation))
                                                                .thenAccept(
                                                                        read ->
                                                                                items.add(
                                                                                        new ZLinkAuthorityEntry(
                                                                                                authorityKey,
                                                                                                (ZLinkAuthoritySnapshot)
                                                                                                        read))));
                            }
                            return visible.thenApply(
                                    ignored ->
                                            new ZLinkAuthorityPage(
                                                    List.copyOf(items),
                                                    page.nextCursor() == null
                                                            ? Optional.empty()
                                                            : Optional.of(
                                                                    new ZLinkAuthorityScanCursor(
                                                                            page.nextCursor()
                                                                                    .value()))));
                        });
    }

    CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request, ZLinkStoreCancellation cancellation) {
        return reserve(request, cancellation, UUID.randomUUID().toString());
    }

    private CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation,
            String reservationVersion) {
        return reserve(request, cancellation, reservationVersion, null);
    }

    private CompletionStage<ZLinkObjectReserveResult> reserve(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation,
            String reservationVersion,
            String reclaimVersion) {
        Objects.requireNonNull(request, "request");
        var opaqueCancellation = adapt(cancellation);
        ZLinkStoreKey key = authorityKey(request.authorityKey());
        return provider.read(key, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (read instanceof ZLinkStoreReadFound found) {
                                AuthorityRecord record = decode(found.value().bytes());
                                ZLinkAuthoritySnapshot current = snapshot(found.value());
                                if (current.allocation().objectKind() != request.objectKind()
                                        || !current.allocation()
                                                .stableType()
                                                .equals(request.stableType()))
                                    return completed(new ZLinkObjectTypeMismatch(current));
                                if (reclaimVersion != null
                                        && !current.storeVersion().equals(reclaimVersion))
                                    return completed(new ZLinkObjectConflict(current));
                                return tryReclaimStaleAuthority(
                                                key,
                                                found,
                                                record,
                                                opaqueCancellation,
                                                request,
                                                null,
                                                null,
                                                null)
                                        .thenCompose(
                                                reclaim ->
                                                        switch (reclaim) {
                                                            case RECLAIMED ->
                                                                    reserve(
                                                                            request,
                                                                            cancellation,
                                                                            reservationVersion);
                                                            case CONFLICT ->
                                                                    reserve(
                                                                            request,
                                                                            cancellation,
                                                                            reservationVersion,
                                                                            current.storeVersion());
                                                            case RECOVERY_REQUIRED ->
                                                                    completed(
                                                                            new ZLinkObjectConflict(
                                                                                    current));
                                                            case OWNER_LIVE, NOT_RECLAIMABLE ->
                                                                    record.aggregate() != null
                                                                            ? projectRead(
                                                                                            read,
                                                                                            opaqueCancellation)
                                                                                    .thenApply(
                                                                                            visible ->
                                                                                                    new ZLinkObjectConflict(
                                                                                                            (ZLinkAuthoritySnapshot)
                                                                                                                    visible))
                                                                            : completed(
                                                                                    current.allocation()
                                                                                                            .state()
                                                                                                    == ZLinkPlacementAllocationState
                                                                                                            .ACTIVE
                                                                                            ? new ZLinkObjectAlreadyExists(
                                                                                                    current)
                                                                                            : new ZLinkObjectConflict(
                                                                                                    current));
                                                        });
                            }
                            if (reclaimVersion != null)
                                return completed(new ZLinkObjectConflict(toRead(read)));
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            conditions.add(new ZLinkStoreMissingCondition(key));
                            return requireLiveOwner(
                                            request.targetOwner(), conditions, opaqueCancellation)
                                    .thenCompose(
                                            live ->
                                                    live
                                                            ? reserveMissing(
                                                                    request,
                                                                    cancellation,
                                                                    key,
                                                                    reservationVersion,
                                                                    conditions,
                                                                    opaqueCancellation)
                                                            : completed(
                                                                    new ZLinkObjectConflict(
                                                                            toRead(read))));
                        });
    }

    private CompletionStage<ZLinkObjectReserveResult> reserveMissing(
            ZLinkObjectReservationRequest request,
            ZLinkStoreCancellation cancellation,
            ZLinkStoreKey key,
            String reservationVersion,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation opaqueCancellation) {
        return descriptors
                .readMeshNode(request.targetDescriptor(), conditions, opaqueCancellation)
                .thenCompose(
                        descriptor -> {
                            if (descriptor.isEmpty()
                                    || !descriptorAdmits(descriptor.orElseThrow(), request)) {
                                return completed(
                                        new ZLinkObjectConflict(
                                                new ZLinkAuthorityMissing(Instant.now())));
                            }
                            List<ZLinkStoreMutation> mutations = new ArrayList<>();
                            if (!ZLinkCapacityCounters.reserve(
                                    descriptor.orElseThrow(),
                                    request.capacityBundle(),
                                    conditions,
                                    mutations)) {
                                return completed(
                                        new ZLinkObjectConflict(
                                                new ZLinkAuthorityMissing(Instant.now())));
                            }
                            return nextPair(request.authorityKey(), conditions, opaqueCancellation)
                                    .thenCompose(
                                            counters -> {
                                                if (counters.exhausted()) {
                                                    return completed(
                                                            new ZLinkObjectGenerationExhausted());
                                                }
                                                var allocation =
                                                        new ZLinkPlacementAllocation(
                                                                ZLinkPlacementAllocationState
                                                                        .PENDING,
                                                                request.objectKind(),
                                                                request.stableType(),
                                                                request.targetDescriptor(),
                                                                request
                                                                        .targetDescriptorLifecycleGeneration(),
                                                                request.capacityBundle());
                                                var creation =
                                                        new ZLinkPendingObjectCreation(
                                                                reservationVersion,
                                                                request.creationIntentReference(),
                                                                request.creationIntentHash(),
                                                                request
                                                                        .creationIntentEncodedSize());
                                                var record =
                                                        new AuthorityRecord(
                                                                request.creatingPayload(),
                                                                counters.objectGeneration(),
                                                                counters.ownerGeneration(),
                                                                request.targetOwner().ownerId(),
                                                                request.targetOwner()
                                                                        .leaseGeneration(),
                                                                allocation,
                                                                Optional.of(creation));
                                                mutations.addAll(counters.mutations());
                                                mutations.add(
                                                        new ZLinkStorePut(
                                                                key, encode(record), null));
                                                return write(
                                                                new ZLinkStoreWriteRequest(
                                                                        conditions, mutations),
                                                                opaqueCancellation)
                                                        .thenCompose(
                                                                result -> {
                                                                    if (result
                                                                            instanceof
                                                                            ZLinkStoreWriteApplied
                                                                                    applied) {
                                                                        return completed(
                                                                                new ZLinkObjectReserved(
                                                                                        new ZLinkObjectReservation(
                                                                                                request
                                                                                                        .authorityKey(),
                                                                                                applied.putVersions()
                                                                                                        .get(
                                                                                                                key)
                                                                                                        .value(),
                                                                                                counters
                                                                                                        .objectGeneration(),
                                                                                                counters
                                                                                                        .ownerGeneration(),
                                                                                                reservationVersion,
                                                                                                request
                                                                                                        .targetDescriptor(),
                                                                                                request
                                                                                                        .targetDescriptorLifecycleGeneration(),
                                                                                                request
                                                                                                        .targetOwner())));
                                                                    }
                                                                    return canRebuild(
                                                                                    key,
                                                                                    null,
                                                                                    null,
                                                                                    request
                                                                                            .targetOwner(),
                                                                                    opaqueCancellation)
                                                                            .thenComposeAsync(
                                                                                    eligible -> {
                                                                                        if (!eligible) {
                                                                                            return provider.read(
                                                                                                            key,
                                                                                                            opaqueCancellation)
                                                                                                    .thenCompose(
                                                                                                            current ->
                                                                                                                    projectRead(
                                                                                                                            current,
                                                                                                                            opaqueCancellation))
                                                                                                    .thenApply(
                                                                                                            ZLinkObjectConflict
                                                                                                                    ::new);
                                                                                        }
                                                                                        return capacityAvailable(
                                                                                                        conditions,
                                                                                                        opaqueCancellation)
                                                                                                .thenCompose(
                                                                                                        available ->
                                                                                                                available
                                                                                                                        ? reserve(
                                                                                                                                request,
                                                                                                                                cancellation,
                                                                                                                                reservationVersion)
                                                                                                                        : completed(
                                                                                                                                new ZLinkPlacementCapacityExhausted()));
                                                                                    });
                                                                });
                                            });
                        });
    }

    private CompletionStage<Boolean> capacityAvailable(
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        var limits =
                conditions.stream().filter(ZLinkStoreSumAtMostCondition.class::isInstance).toList();
        if (limits.isEmpty()) return completed(true);
        // Batch conflicts do not identify which condition failed. Recheck just the limits
        // atomically, without mutating capacity or any authority.
        return write(new ZLinkStoreWriteRequest(limits, List.of()), cancellation)
                .thenApply(ZLinkStoreWriteApplied.class::isInstance);
    }

    CompletionStage<Boolean> cleanupDiscarded(
            String key,
            ZLinkAuthoritySnapshot discarded,
            ZLinkLocationOwnerToken currentOwner,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(discarded, "discarded");
        Objects.requireNonNull(currentOwner, "currentOwner");
        if (!currentOwner.ownerId().equals(discarded.ownerId()))
            throw new IllegalArgumentException("Discard cleanup requires the same owner identity");
        return releaseEndedReservation(
                key, null, adapt(cancellation), null, null, discarded, currentOwner);
    }

    CompletionStage<Boolean> releaseEndedReservation(
            String key,
            String expectedStoreVersion,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return releaseEndedReservation(
                key, expectedStoreVersion, cancellation, null, null, null, null);
    }

    CompletionStage<Boolean> releaseEndedReservation(
            ZLinkObjectReservationRequest request,
            String expectedStoreVersion,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return releaseEndedReservation(
                request.authorityKey(),
                expectedStoreVersion,
                cancellation,
                request,
                null,
                null,
                null);
    }

    private CompletionStage<Boolean> releaseEndedReservation(
            String key,
            String expectedStoreVersion,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            ZLinkObjectReservationRequest request,
            ZLinkLocationOwnerToken requiredOwner,
            ZLinkAuthoritySnapshot discarded,
            ZLinkLocationOwnerToken currentOwner) {
        ZLinkStoreKey rowKey = authorityKey(key);
        return provider.read(rowKey, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found))
                                return completed(discarded != null);
                            if (discarded != null
                                    && !discarded.sameIncarnation(snapshot(found.value())))
                                return completed(true);
                            if ((discarded == null || expectedStoreVersion != null)
                                    && !canRebuild(read, expectedStoreVersion, null))
                                return completed(false);
                            var current = decode(found.value().bytes());
                            if (request != null
                                    && (current.allocation().objectKind() != request.objectKind()
                                            || !current.allocation()
                                                    .stableType()
                                                    .equals(request.stableType())))
                                return completed(false);
                            return tryReclaimStaleAuthority(
                                            rowKey,
                                            found,
                                            current,
                                            cancellation,
                                            request,
                                            requiredOwner,
                                            discarded == null ? null : currentOwner,
                                            key)
                                    .thenComposeAsync(
                                            result ->
                                                    result == StaleAuthorityReclaim.CONFLICT
                                                            ? releaseEndedReservation(
                                                                    key,
                                                                    found.value().version().value(),
                                                                    cancellation,
                                                                    request,
                                                                    requiredOwner,
                                                                    discarded,
                                                                    currentOwner)
                                                            : completed(
                                                                    result
                                                                            == StaleAuthorityReclaim
                                                                                    .RECLAIMED));
                        });
    }

    private CompletionStage<StaleAuthorityReclaim> tryReclaimStaleAuthority(
            ZLinkStoreKey authorityKey,
            ZLinkStoreReadFound authority,
            AuthorityRecord current,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            ZLinkObjectReservationRequest request,
            ZLinkLocationOwnerToken requiredOwner,
            ZLinkLocationOwnerToken discardedOwner,
            String discardedKey) {
        boolean discarded = discardedOwner != null;
        boolean active = current.allocation().state() == ZLinkPlacementAllocationState.ACTIVE;
        boolean actorClosing =
                ZLinkAuthoritySnapshot.actorClosing(current.payload(), current.allocation());
        if ((active
                        ? request == null && !discarded && !actorClosing
                        : discarded || current.pendingCreation().isEmpty())
                || (!active
                        && (current.aggregate() != null
                                || current.relocation() != null
                                || ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                                                current.payload())
                                        != null)))
            return completed(StaleAuthorityReclaim.NOT_RECLAIMABLE);
        boolean steadyReady =
                active
                        && current.aggregate() == null
                        && current.relocation() == null
                        && current.pendingCreation().isEmpty()
                        && ZLinkAuthoritySnapshot.steadyReady(
                                current.payload(), current.allocation());
        CompletionStage<ZLinkAuthorityReadResult> evidence =
                current.aggregate() == null
                        ? completed(snapshot(authority.value()))
                        : projectRead(authority, cancellation);
        return evidence.thenCompose(
                projected -> {
                    var ownerEvidence = (ZLinkAuthoritySnapshot) projected;
                    var owner =
                            new ZLinkLocationOwnerToken(
                                    ownerEvidence.ownerId(), ownerEvidence.ownerLeaseGeneration());
                    var source = current.allocation().descriptor();
                    var sourceKey =
                            ZLinkProviderDescriptorRepository.meshKey(
                                    source.meshName(), source.rid());
                    return provider.read(ownerKey(owner.ownerId()), cancellation)
                            .thenCompose(
                                    lease -> {
                                        boolean ownerLive =
                                                ZLinkProviderOwnerLeaseRepository.isLive(
                                                        lease,
                                                        owner.ownerId(),
                                                        owner.leaseGeneration());
                                        if (requiredOwner != null
                                                && (!ownerLive || !requiredOwner.equals(owner))) {
                                            return completed(StaleAuthorityReclaim.NOT_RECLAIMABLE);
                                        }
                                        boolean authorityOwnerWaiting =
                                                active
                                                        && !ownerLive
                                                        && (discarded || steadyReady)
                                                        && ownerLeaseWait.shouldWait(owner);
                                        if (active) {
                                            if (ownerLive && !owner.equals(discardedOwner)) {
                                                return completed(StaleAuthorityReclaim.OWNER_LIVE);
                                            }
                                            if (current.aggregate() != null
                                                    || current.relocation() != null
                                                    || ZLinkCanonicalRelocationAuthorityStateCodec
                                                                    .decode(current.payload())
                                                            != null) {
                                                throw new ZLinkFrameworkException(
                                                        ZLinkFrameworkErrorKind.UNAVAILABLE,
                                                        "Ended authority requires relocation recovery");
                                            }
                                        }
                                        return provider.read(sourceKey, cancellation)
                                                .thenCompose(
                                                        descriptor -> {
                                                            ZLinkStoreCondition sourceCondition;
                                                            if (descriptor
                                                                    instanceof
                                                                    ZLinkStoreReadFound found) {
                                                                if (!active
                                                                        && ownerLive
                                                                        && ZLinkProviderDescriptorRepository
                                                                                        .decodeMeshNodeRecord(
                                                                                                found.value()
                                                                                                        .bytes())
                                                                                        .lifecycleGeneration()
                                                                                == current.allocation()
                                                                                        .descriptorLifecycleGeneration()) {
                                                                    return completed(
                                                                            StaleAuthorityReclaim
                                                                                    .OWNER_LIVE);
                                                                }
                                                                sourceCondition =
                                                                        new ZLinkStoreVersionCondition(
                                                                                sourceKey,
                                                                                found.value()
                                                                                        .version());
                                                            } else {
                                                                sourceCondition =
                                                                        new ZLinkStoreMissingCondition(
                                                                                sourceKey);
                                                            }
                                                            return creationPolicyCondition(
                                                                            active,
                                                                            discarded
                                                                                    || actorClosing,
                                                                            request,
                                                                            sourceKey,
                                                                            descriptor,
                                                                            cancellation)
                                                                    .thenCompose(
                                                                            targetCondition ->
                                                                                    readEndedMembers(
                                                                                                    current,
                                                                                                    active,
                                                                                                    discarded,
                                                                                                    cancellation)
                                                                                            .thenCompose(
                                                                                                    ended -> {
                                                                                                        if (discarded
                                                                                                                && (authorityOwnerWaiting
                                                                                                                        || !ended
                                                                                                                                .isEmpty())) {
                                                                                                            return completed(
                                                                                                                    StaleAuthorityReclaim
                                                                                                                            .NOT_RECLAIMABLE);
                                                                                                        }
                                                                                                        if (discarded
                                                                                                                && ownerLive) {
                                                                                                            return compareExchange(
                                                                                                                            discardedKey,
                                                                                                                            new ZLinkAuthorityExpectFound(
                                                                                                                                    authority
                                                                                                                                            .value()
                                                                                                                                            .version()
                                                                                                                                            .value()),
                                                                                                                            new ZLinkAuthorityDelete(),
                                                                                                                            cancellation
                                                                                                                                    ::isCancellationRequested)
                                                                                                                    .thenApply(
                                                                                                                            result ->
                                                                                                                                    result
                                                                                                                                                    instanceof
                                                                                                                                                    ZLinkAuthorityDeleted
                                                                                                                                            ? StaleAuthorityReclaim
                                                                                                                                                    .RECLAIMED
                                                                                                                                            : StaleAuthorityReclaim
                                                                                                                                                    .CONFLICT);
                                                                                                        }
                                                                                                        return releaseEndedAuthority(
                                                                                                                authorityKey,
                                                                                                                authority,
                                                                                                                current,
                                                                                                                active,
                                                                                                                owner
                                                                                                                        .ownerId(),
                                                                                                                lease,
                                                                                                                sourceCondition,
                                                                                                                targetCondition,
                                                                                                                ended,
                                                                                                                authorityOwnerWaiting,
                                                                                                                cancellation);
                                                                                                    }));
                                                        });
                                    });
                });
    }

    private CompletionStage<List<EndedMember>> readEndedMembers(
            AuthorityRecord current,
            boolean active,
            boolean discarded,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (!active || current.allocation().objectKind() == ZLinkPlacementObjectKind.ACTOR) {
            return completed(List.of());
        }
        var spot = new ZLinkServiceAuthorityPayloadCodec().decode(current.payload());
        if (spot.isEmpty()
                || (!discarded
                        && (spot.get().state() != ZLinkServiceAuthorityPayloadCodec.State.READY
                                || spot.get().activationRecoveryState().isPresent()))) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE, "Ended Spot authority is not reclaimable");
        }
        if (spot.get().user().isEmpty()) return completed(List.of());
        return readEndedSpotMembers(
                spot.get().spotId(),
                current.objectGeneration(),
                null,
                new ArrayList<>(),
                cancellation,
                !discarded);
    }

    private CompletionStage<ZLinkStoreCondition> creationPolicyCondition(
            boolean active,
            boolean policyExempt,
            ZLinkObjectReservationRequest request,
            ZLinkStoreKey sourceKey,
            ZLinkStoreReadResult source,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (!active || policyExempt) return completed(null);
        var target = request.targetDescriptor();
        var targetKey = ZLinkProviderDescriptorRepository.meshKey(target.meshName(), target.rid());
        var sameKey = targetKey.equals(sourceKey);
        return (sameKey ? completed(source) : provider.read(targetKey, cancellation))
                .thenApply(
                        targetRead -> {
                            var found = targetRead instanceof ZLinkStoreReadFound row ? row : null;
                            ZLinkAuthorityCleanup.requireDisabledCreationPolicy(
                                    request,
                                    found == null
                                            ? null
                                            : ZLinkProviderDescriptorRepository
                                                    .decodeMeshNodeRecord(found.value().bytes()));
                            return sameKey
                                    ? null
                                    : new ZLinkStoreVersionCondition(
                                            targetKey, found.value().version());
                        });
    }

    /** An Actor membership of an ended User Spot incarnation, read from one scan snapshot. */
    private record EndedMember(
            ZLinkStoreKey key, ZLinkStoreVersion version, AuthorityRecord record) {}

    /**
     * Reads every Actor membership of one User Spot incarnation from one complete snapshot scan
     * (location runtime §6.1). An expired snapshot discards the earlier pages and restarts; a Store
     * failure propagates. A member that is relocating, part of an aggregate or not yet activated
     * keeps the Spot.
     */
    private CompletionStage<List<EndedMember>> readEndedSpotMembers(
            String spotId,
            long generation,
            ZLinkStoreScanCursor cursor,
            List<EndedMember> members,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean requireSteadyReady) {
        return provider.scan(
                        new ZLinkStoreScanRequest(ACTOR_AUTHORITY_PREIMAGE_PREFIX, cursor, 1000),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired)
                                return readEndedSpotMembers(
                                        spotId,
                                        generation,
                                        null,
                                        new ArrayList<>(),
                                        cancellation,
                                        requireSteadyReady);
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            CompletionStage<Void> pageRead = completed(null);
                            for (var item : page.items()) {
                                var record = decode(item.value().bytes());
                                if (record.allocation().objectKind()
                                        != ZLinkPlacementObjectKind.ACTOR) continue;
                                pageRead =
                                        pageRead.thenCompose(
                                                ignored ->
                                                        projectRead(
                                                                        new ZLinkStoreReadFound(
                                                                                item.value()),
                                                                        cancellation)
                                                                .thenAccept(
                                                                        read -> {
                                                                            if (!(read
                                                                                            instanceof
                                                                                            ZLinkAuthoritySnapshot
                                                                                                    snapshot)
                                                                                    || !isSpotMember(
                                                                                            snapshot
                                                                                                    .payload(),
                                                                                            spotId,
                                                                                            generation))
                                                                                return;
                                                                            if (requireSteadyReady
                                                                                    && !isSteadyReadyMember(
                                                                                            record))
                                                                                throw new ZLinkFrameworkException(
                                                                                        ZLinkFrameworkErrorKind
                                                                                                .UNAVAILABLE,
                                                                                        "User Spot"
                                                                                                + " still"
                                                                                                + " has a"
                                                                                                + " relocating"
                                                                                                + " or unactivated"
                                                                                                + " member");
                                                                            members.add(
                                                                                    new EndedMember(
                                                                                            item
                                                                                                    .key(),
                                                                                            item.value()
                                                                                                    .version(),
                                                                                            record));
                                                                        }));
                            }
                            return pageRead.thenCompose(
                                    ignored ->
                                            page.nextCursor() == null
                                                    ? completed(members)
                                                    : readEndedSpotMembers(
                                                            spotId,
                                                            generation,
                                                            page.nextCursor(),
                                                            members,
                                                            cancellation,
                                                            requireSteadyReady));
                        });
    }

    private boolean requireEndedMemberOwnerWaitElapsed(
            EndedMember member, ZLinkStoreReadResult ownerRead) {
        var owner =
                new ZLinkLocationOwnerToken(
                        member.record().ownerId(), member.record().ownerLeaseGeneration());
        boolean live =
                ZLinkProviderOwnerLeaseRepository.isLive(
                        ownerRead, owner.ownerId(), owner.leaseGeneration());
        return !live && ownerLeaseWait.shouldWait(owner);
    }

    private static boolean isSpotMember(byte[] payload, String spotId, long generation) {
        return new ZLinkActorAuthorityPayloadCodec()
                .decode(payload)
                .filter(
                        actor ->
                                actor.currentSpotKind()
                                                == systems.zlink.framework.spots.ZLinkSpotKind.USER
                                                        .value()
                                        && actor.currentSpotId().equals(spotId)
                                        && actor.currentSpotGeneration() == generation)
                .isPresent();
    }

    /** Steady Ready: activated, outside any aggregate, no relocation in progress, Ready payload. */
    private static boolean isSteadyReadyMember(AuthorityRecord record) {
        return record.aggregate() == null
                && record.relocation() == null
                && record.pendingCreation().isEmpty()
                && record.allocation().objectKind() == ZLinkPlacementObjectKind.ACTOR
                && ZLinkAuthoritySnapshot.steadyReady(record.payload(), record.allocation());
    }

    /**
     * Releases an ended authority in one conditional batch (location runtime §6.1). For a User Spot
     * the batch also releases every ended-owner member: each member authority is fenced by its
     * Store version and the observed (invalid) state of its owner lease, and its capacity is
     * returned. A member with a live owner lease, or a batch beyond the provider bound, changes
     * nothing and is Unavailable.
     */
    private CompletionStage<StaleAuthorityReclaim> releaseEndedAuthority(
            ZLinkStoreKey authorityKey,
            ZLinkStoreReadFound authority,
            AuthorityRecord current,
            boolean active,
            String ownerId,
            ZLinkStoreReadResult ownerLease,
            ZLinkStoreCondition descriptorCondition,
            ZLinkStoreCondition targetCondition,
            List<EndedMember> members,
            boolean authorityOwnerWaiting,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        Map<String, ZLinkStoreReadResult> owners = new java.util.LinkedHashMap<>();
        owners.put(ownerId, ownerLease);
        CompletionStage<Boolean> ownersRead = completed(authorityOwnerWaiting);
        for (var member : members) {
            String memberOwner = member.record().ownerId();
            ownersRead =
                    ownersRead.thenCompose(
                            waitingSoFar -> {
                                ZLinkStoreReadResult existing = owners.get(memberOwner);
                                CompletionStage<ZLinkStoreReadResult> read =
                                        existing == null
                                                ? provider.read(ownerKey(memberOwner), cancellation)
                                                : completed(existing);
                                return read.thenApply(
                                        ownerRead -> {
                                            boolean memberWaiting =
                                                    requireEndedMemberOwnerWaitElapsed(
                                                            member, ownerRead);
                                            owners.putIfAbsent(memberOwner, ownerRead);
                                            return waitingSoFar || memberWaiting;
                                        });
                            });
        }
        return ownersRead.thenCompose(
                waiting -> {
                    if (waiting)
                        throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "Owner lease is within its authority release wait");
                    List<ZLinkStoreCondition> conditions = new ArrayList<>();
                    List<ZLinkStoreMutation> mutations = new ArrayList<>();
                    conditions.add(
                            new ZLinkStoreVersionCondition(
                                    authorityKey, authority.value().version()));
                    conditions.add(descriptorCondition);
                    if (targetCondition != null) conditions.add(targetCondition);
                    mutations.add(new ZLinkStoreDelete(authorityKey));
                    for (var member : members) {
                        if (ZLinkProviderOwnerLeaseRepository.isLive(
                                owners.get(member.record().ownerId()),
                                member.record().ownerId(),
                                member.record().ownerLeaseGeneration()))
                            throw new ZLinkFrameworkException(
                                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                                    "User Spot still has a member with a live owner lease");
                        conditions.add(
                                new ZLinkStoreVersionCondition(member.key(), member.version()));
                        mutations.add(new ZLinkStoreDelete(member.key()));
                    }
                    owners.forEach(
                            (id, read) ->
                                    conditions.add(
                                            read instanceof ZLinkStoreReadFound found
                                                    ? new ZLinkStoreVersionCondition(
                                                            ownerKey(id), found.value().version())
                                                    : new ZLinkStoreMissingCondition(
                                                            ownerKey(id))));
                    var deltas = new ZLinkCapacityCounters();
                    deltas.change(
                            current.allocation().descriptor(),
                            current.allocation().capacityBundle(),
                            active ? 0 : -1,
                            active ? -1 : 0);
                    for (var member : members) {
                        var allocation = member.record().allocation();
                        deltas.change(allocation.descriptor(), allocation.capacityBundle(), 0, -1);
                    }
                    deltas.appendTo(mutations);
                    var keys = new java.util.HashSet<ZLinkStoreKey>();
                    conditions.forEach(condition -> keys.addAll(condition.keys()));
                    mutations.forEach(mutation -> keys.add(mutation.key()));
                    if (keys.size() > ZLinkLocationStoreLimits.MAXIMUM_BATCH_KEYS)
                        throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "User Spot member release exceeds one Location Store batch");
                    return write(new ZLinkStoreWriteRequest(conditions, mutations), cancellation)
                            .thenApply(
                                    result -> {
                                        if (!(result instanceof ZLinkStoreWriteApplied))
                                            return StaleAuthorityReclaim.CONFLICT;
                                        return StaleAuthorityReclaim.RECLAIMED;
                                    });
                });
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation, byte[] readyPayload, long deadlineUnixMs) {
        return commit(reservation, readyPayload, null, deadlineUnixMs);
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            long deadlineUnixMs) {
        return commit(
                reservation,
                readyPayload,
                terminal,
                () -> {
                    if (System.currentTimeMillis() >= deadlineUnixMs)
                        throw new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                                "Instance activation deadline expired");
                    return false;
                });
    }

    CompletionStage<ZLinkObjectCommitResult> commit(
            ZLinkObjectReservation reservation,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(reservation, "reservation");
        Objects.requireNonNull(readyPayload, "readyPayload");
        if (terminal != null) {
            requireTerminal(reservation, terminal, ZLinkCreationTerminalState.CREATED);
        }
        ZLinkStoreKey key = authorityKey(reservation.authorityKey());
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(ZLinkObjectCommitResult.STALE);
                            }
                            AuthorityRecord current = decode(found.value().bytes());
                            if (current.aggregate() != null) {
                                return completed(ZLinkObjectCommitResult.STALE);
                            }
                            if (!matches(found.value(), current, reservation)) {
                                return completed(
                                        current.pendingCreation().isEmpty()
                                                ? ZLinkObjectCommitResult.ALREADY_COMMITTED
                                                : ZLinkObjectCommitResult.STALE);
                            }
                            return writeCreationTransition(
                                            key,
                                            found.value(),
                                            current,
                                            readyPayload,
                                            terminal,
                                            opaqueCancellation)
                                    .thenApply(
                                            applied ->
                                                    applied
                                                            ? ZLinkObjectCommitResult.COMMITTED
                                                            : ZLinkObjectCommitResult.STALE);
                        });
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation, ZLinkStoreCancellation cancellation) {
        return abortCreation(reservation, null, cancellation);
    }

    CompletionStage<ZLinkObjectAbortResult> abort(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        requireTerminal(reservation, terminal, ZLinkCreationTerminalState.FAILED);
        return abortCreation(reservation, terminal, cancellation);
    }

    private CompletionStage<ZLinkObjectAbortResult> abortCreation(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(reservation, "reservation");
        ZLinkStoreKey key = authorityKey(reservation.authorityKey());
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(ZLinkObjectAbortResult.STALE);
                            }
                            AuthorityRecord current = decode(found.value().bytes());
                            if (current.aggregate() != null
                                    || current.relocation() != null
                                    || !matches(found.value(), current, reservation)) {
                                return completed(ZLinkObjectAbortResult.STALE);
                            }
                            return writeCreationTransition(
                                            key,
                                            found.value(),
                                            current,
                                            null,
                                            terminal,
                                            opaqueCancellation)
                                    .thenApply(
                                            applied ->
                                                    applied
                                                            ? ZLinkObjectAbortResult.ABORTED
                                                            : ZLinkObjectAbortResult.STALE);
                        });
    }

    CompletionStage<ZLinkObjectRejectResult> reject(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkStoreCancellation cancellation) {
        requireTerminal(reservation, terminal, ZLinkCreationTerminalState.REJECTED);
        return abortCreation(reservation, terminal, cancellation)
                .thenApply(
                        result ->
                                switch (result) {
                                    case ABORTED -> ZLinkObjectRejectResult.REJECTED;
                                    case ALREADY_ABORTED ->
                                            ZLinkObjectRejectResult.ALREADY_REJECTED;
                                    case STALE -> ZLinkObjectRejectResult.STALE;
                                });
    }

    private CompletionStage<Boolean> writeCreationTransition(
            ZLinkStoreKey key,
            ZLinkStoreValue authority,
            AuthorityRecord current,
            byte[] readyPayload,
            ZLinkCreationOperationTerminal terminal,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return completed(false);
        }
        List<ZLinkStoreCondition> conditions =
                new ArrayList<>(List.of(new ZLinkStoreVersionCondition(key, authority.version())));
        ZLinkLocationOwnerToken owner =
                new ZLinkLocationOwnerToken(current.ownerId(), current.ownerLeaseGeneration());
        return descriptors
                .readMeshNode(current.allocation().descriptor(), conditions, cancellation)
                .thenCompose(
                        descriptor -> {
                            if (descriptor.isEmpty()
                                    || !descriptorMatchesTarget(
                                            descriptor.orElseThrow(),
                                            current.allocation().descriptor(),
                                            current.allocation().descriptorLifecycleGeneration(),
                                            owner)) {
                                return completed(false);
                            }
                            List<ZLinkStoreMutation> mutations = new ArrayList<>();
                            if (readyPayload == null) {
                                mutations.add(new ZLinkStoreDelete(key));
                            } else {
                                AuthorityRecord next =
                                        new AuthorityRecord(
                                                readyPayload,
                                                current.objectGeneration(),
                                                current.authorityOwnerGeneration(),
                                                current.ownerId(),
                                                current.ownerLeaseGeneration(),
                                                withState(
                                                        current.allocation(),
                                                        ZLinkPlacementAllocationState.ACTIVE),
                                                Optional.empty());
                                mutations.add(new ZLinkStorePut(key, encode(next), null));
                            }
                            var deltas = new ZLinkCapacityCounters();
                            deltas.change(
                                    current.allocation().descriptor(),
                                    current.allocation().capacityBundle(),
                                    -1,
                                    readyPayload == null ? 0 : 1);
                            deltas.appendTo(mutations);
                            if (terminal != null) {
                                Duration retention =
                                        Duration.between(
                                                authority.storeNow(), terminal.expiresAt());
                                if (retention.isZero() || retention.isNegative()) {
                                    throw new IllegalArgumentException(
                                            "terminal expiresAt must be later than provider store"
                                                    + " time");
                                }
                                ZLinkStoreKey terminalKey =
                                        creationTerminalKey(terminal.operation());
                                conditions.add(new ZLinkStoreMissingCondition(terminalKey));
                                // A full 1 MiB envelope needs its own provider value.
                                mutations.add(
                                        new ZLinkStorePut(
                                                terminalKey,
                                                terminal.terminalEnvelope(),
                                                retention));
                            }
                            return requireLiveOwner(owner, conditions, cancellation)
                                    .thenCompose(
                                            live ->
                                                    live && readyPayload != null
                                                            ? spotMembershipFailure(
                                                                            current.payload(),
                                                                            readyPayload,
                                                                            conditions,
                                                                            cancellation)
                                                                    .thenApply(
                                                                            failure ->
                                                                                    failure == null)
                                                            : completed(live))
                                    .thenCompose(
                                            live -> {
                                                if (!live) {
                                                    return completed(false);
                                                }
                                                return write(
                                                                new ZLinkStoreWriteRequest(
                                                                        conditions, mutations),
                                                                cancellation)
                                                        .thenCompose(
                                                                result -> {
                                                                    if (result
                                                                            instanceof
                                                                            ZLinkStoreWriteApplied) {
                                                                        return completed(true);
                                                                    }
                                                                    return canRebuild(
                                                                                    key,
                                                                                    authority
                                                                                            .version()
                                                                                            .value(),
                                                                                    current.pendingCreation()
                                                                                            .map(
                                                                                                    ZLinkPendingObjectCreation
                                                                                                            ::reservationId)
                                                                                            .orElse(
                                                                                                    null),
                                                                                    owner,
                                                                                    cancellation)
                                                                            .thenCompose(
                                                                                    eligible -> {
                                                                                        if (!eligible
                                                                                                || terminal
                                                                                                        == null) {
                                                                                            return completed(
                                                                                                    eligible);
                                                                                        }
                                                                                        return provider.read(
                                                                                                        creationTerminalKey(
                                                                                                                terminal
                                                                                                                        .operation()),
                                                                                                        cancellation)
                                                                                                .thenApply(
                                                                                                        read ->
                                                                                                                read
                                                                                                                        instanceof
                                                                                                                        ZLinkStoreReadMissing);
                                                                                    })
                                                                            .thenComposeAsync(
                                                                                    eligible ->
                                                                                            eligible
                                                                                                    ? writeCreationTransition(
                                                                                                            key,
                                                                                                            authority,
                                                                                                            current,
                                                                                                            readyPayload,
                                                                                                            terminal,
                                                                                                            cancellation)
                                                                                                    : completed(
                                                                                                            false));
                                                                });
                                            });
                        });
    }

    CompletionStage<ZLinkCreationTerminalReadResult> readCreationTerminal(
            ZLinkCreationOperationIdentity operation, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(operation, "operation");
        return provider.read(creationTerminalKey(operation), adapt(cancellation))
                .thenApply(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return new ZLinkCreationTerminalMissing();
                            }
                            return new ZLinkCreationTerminalFound(found.value().bytes());
                        });
    }

    private static void requireTerminal(
            ZLinkObjectReservation reservation,
            ZLinkCreationOperationTerminal terminal,
            ZLinkCreationTerminalState expectedState) {
        Objects.requireNonNull(terminal, "terminal");
        if (!terminal.reservation().equals(reservation)) {
            throw new IllegalArgumentException(
                    "terminal reservation must match the exact reservation");
        }
        if (terminal.state() != expectedState) {
            throw new IllegalArgumentException("terminal state must be " + expectedState);
        }
    }

    private static ZLinkStoreKey creationTerminalKey(ZLinkCreationOperationIdentity operation) {
        return ZLinkOpaqueRecordKey.of(
                "creation-terminal",
                operation.sourceNodeRid().toHex(),
                Long.toUnsignedString(operation.sourceLifecycleGeneration()),
                HexFormat.of().toHexDigits(operation.operationIdHigh())
                        + HexFormat.of().toHexDigits(operation.operationIdLow()));
    }

    CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request, ZLinkStoreCancellation cancellation) {
        return prepareAggregate(
                request, cancellation, Instant.now().plus(AGGREGATE_PREPARE_RETRY_WINDOW));
    }

    private CompletionStage<ZLinkAggregatePrepareResult> prepareAggregate(
            ZLinkAggregatePrepareRequest request,
            ZLinkStoreCancellation cancellation,
            Instant retryDeadline) {
        Objects.requireNonNull(request, "request");
        ZLinkAggregateFence fence =
                new ZLinkAggregateFence(request.aggregateId(), request.aggregateGeneration());
        ZLinkStoreKey key = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation)
                .thenCompose(
                        existing -> {
                            if (existing instanceof ZLinkStoreReadFound found) {
                                PreparedAggregate prepared = decodeAggregate(found.value().bytes());
                                if (!sameAggregateRequest(prepared, request)) {
                                    return completed(new ZLinkAggregateConflict());
                                }
                                if (prepared.state() == AGGREGATE_COMMITTED) {
                                    return completed(new ZLinkAggregateStale());
                                }
                                if (prepared.state() == AGGREGATE_PREPARED) {
                                    return aggregateInventory
                                            .load(
                                                    fence,
                                                    prepared.participantCount(),
                                                    prepared.inventoryDigest(),
                                                    opaqueCancellation)
                                            .thenApply(
                                                    ignored ->
                                                            new ZLinkAggregateAlreadyPrepared(
                                                                    fence));
                                }
                                if (prepared.state() != AGGREGATE_STAGING) {
                                    return completed(new ZLinkAggregateConflict());
                                }
                                return continueAggregatePrepare(
                                        request,
                                        fence,
                                        key,
                                        found,
                                        opaqueCancellation,
                                        false,
                                        retryDeadline);
                            }
                            return write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(new ZLinkStoreMissingCondition(key)),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    key,
                                                                    encodeAggregate(
                                                                            AGGREGATE_STAGING,
                                                                            request),
                                                                    null))),
                                            opaqueCancellation)
                                    .thenCompose(
                                            result -> {
                                                if (!(result
                                                        instanceof
                                                        ZLinkStoreWriteApplied applied)) {
                                                    return prepareAggregate(
                                                            request, cancellation, retryDeadline);
                                                }
                                                ZLinkStoreVersion version =
                                                        applied.putVersions().get(key);
                                                if (version == null) {
                                                    return failed(
                                                            new IllegalStateException(
                                                                    "aggregate staging did not"
                                                                            + " return a"
                                                                            + " StoreVersion"));
                                                }
                                                return continueAggregatePrepare(
                                                        request,
                                                        fence,
                                                        key,
                                                        new ZLinkStoreReadFound(
                                                                new ZLinkStoreValue(
                                                                        encodeAggregate(
                                                                                AGGREGATE_STAGING,
                                                                                request),
                                                                        version,
                                                                        null,
                                                                        applied.storeNow())),
                                                        opaqueCancellation,
                                                        true,
                                                        retryDeadline);
                                            });
                        });
    }

    private CompletionStage<ZLinkAggregatePrepareResult> continueAggregatePrepare(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            ZLinkStoreKey key,
            ZLinkStoreReadFound staging,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean ownsStaging,
            Instant retryDeadline) {
        // Decide target admission before any participant marker is written, so a refused
        // relocation leaves every participant authority untouched.
        return reserveAggregateCapacity(request, cancellation)
                .thenCompose(
                        reservation ->
                                reservation.isEmpty()
                                        ? refuseAggregatePrepare(
                                                fence, staging, cancellation, ownsStaging)
                                        : aggregateInventory
                                                .store(request, cancellation)
                                                .thenCompose(
                                                        ignored ->
                                                                installAggregateMarkers(
                                                                        request,
                                                                        fence,
                                                                        cancellation,
                                                                        retryDeadline))
                                                .thenCompose(
                                                        markers ->
                                                                finishAggregatePrepare(
                                                                        request,
                                                                        fence,
                                                                        key,
                                                                        staging,
                                                                        cancellation,
                                                                        ownsStaging,
                                                                        retryDeadline,
                                                                        reservation.orElseThrow(),
                                                                        markers)));
    }

    private CompletionStage<ZLinkAggregatePrepareResult> finishAggregatePrepare(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            ZLinkStoreKey key,
            ZLinkStoreReadFound staging,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean ownsStaging,
            Instant retryDeadline,
            AggregateCapacityReservation reservation,
            AggregateMarkerInstallation markers) {
        if (markers.reenteredResult() != null) {
            ZLinkAggregatePrepareResult reentered = markers.reenteredResult();
            boolean needsCleanup =
                    reentered instanceof ZLinkAggregateConflict
                            || reentered instanceof ZLinkAggregateGenerationExhausted;
            CompletionStage<Boolean> cleanup =
                    ownsStaging && needsCleanup
                            ? abortOwnedAggregateStaging(
                                    fence, staging.value().version().value(), cancellation)
                            : completed(false);
            return cleanup.thenApply(ignored -> reentered);
        }
        if (!markers.installed()) {
            CompletionStage<Boolean> cleanup =
                    ownsStaging
                            ? abortOwnedAggregateStaging(
                                    fence, staging.value().version().value(), cancellation)
                            : completed(false);
            return cleanup.thenApply(
                    ignored ->
                            markers.exhausted()
                                    ? new ZLinkAggregateGenerationExhausted()
                                    : new ZLinkAggregateConflict());
        }
        return publishAggregatePrepared(
                request,
                fence,
                key,
                staging,
                cancellation,
                ownsStaging,
                retryDeadline,
                reservation);
    }

    /**
     * Location runtime §6.1 {@code NewOwner}: the STAGING to PREPARED write is the one place that
     * reserves the relocation's target capacity. The same conditional write holds {@code
     * request.capacityBundle()} as pending usage on the target, so new placements see it from
     * prepare on; commit turns exactly that reservation into active usage and abort returns it. A
     * write that raced another change to the target descriptor or capacity record re-reads them and
     * decides again.
     */
    private CompletionStage<ZLinkAggregatePrepareResult> publishAggregatePrepared(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            ZLinkStoreKey key,
            ZLinkStoreReadFound staging,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean ownsStaging,
            Instant retryDeadline,
            AggregateCapacityReservation reservation) {
        List<ZLinkStoreCondition> conditions = new ArrayList<>();
        conditions.add(new ZLinkStoreVersionCondition(key, staging.value().version()));
        conditions.addAll(reservation.conditions());
        List<ZLinkStoreMutation> mutations = new ArrayList<>();
        mutations.add(new ZLinkStorePut(key, encodeAggregate(AGGREGATE_PREPARED, request), null));
        mutations.addAll(reservation.mutations());
        return write(new ZLinkStoreWriteRequest(conditions, mutations), cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreWriteApplied) {
                                return completed(new ZLinkAggregatePrepared(fence));
                            }
                            return provider.read(key, cancellation)
                                    .thenCompose(
                                            raced ->
                                                    afterAggregatePrepareRace(
                                                            request,
                                                            fence,
                                                            key,
                                                            staging,
                                                            cancellation,
                                                            ownsStaging,
                                                            retryDeadline,
                                                            raced));
                        });
    }

    private CompletionStage<ZLinkAggregatePrepareResult> afterAggregatePrepareRace(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            ZLinkStoreKey key,
            ZLinkStoreReadFound staging,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean ownsStaging,
            Instant retryDeadline,
            ZLinkStoreReadResult raced) {
        if (!(raced instanceof ZLinkStoreReadFound found)) {
            return completed(new ZLinkAggregateConflict());
        }
        PreparedAggregate current = decodeAggregate(found.value().bytes());
        if (!sameAggregateRequest(current, request)) {
            return completed(new ZLinkAggregateConflict());
        }
        if (current.state() == AGGREGATE_PREPARED) {
            return completed(new ZLinkAggregateAlreadyPrepared(fence));
        }
        if (current.state() == AGGREGATE_COMMITTED) {
            return completed(new ZLinkAggregateStale());
        }
        if (!found.value().version().equals(staging.value().version())
                || cancellation.isCancellationRequested()
                || !Instant.now().isBefore(retryDeadline)) {
            return refuseAggregatePrepare(fence, staging, cancellation, ownsStaging);
        }
        return reserveAggregateCapacity(request, cancellation)
                .thenCompose(
                        reservation ->
                                reservation.isEmpty()
                                        ? refuseAggregatePrepare(
                                                fence, staging, cancellation, ownsStaging)
                                        : publishAggregatePrepared(
                                                request,
                                                fence,
                                                key,
                                                staging,
                                                cancellation,
                                                ownsStaging,
                                                retryDeadline,
                                                reservation.orElseThrow()));
    }

    private CompletionStage<Optional<AggregateCapacityReservation>> reserveAggregateCapacity(
            ZLinkAggregatePrepareRequest request,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        ZLinkPlacementCapacityBundle bundle = request.capacityBundle();
        if (isEmptyBundle(bundle)) {
            return completed(Optional.of(new AggregateCapacityReservation(List.of(), List.of())));
        }
        List<ZLinkStoreCondition> conditions = new ArrayList<>();
        return descriptors
                .readMeshNode(request.targetDescriptor(), conditions, cancellation)
                .thenCompose(
                        descriptor -> {
                            if (descriptor.isEmpty()
                                    || !descriptorMatchesTarget(
                                            descriptor.orElseThrow(),
                                            request.targetDescriptor(),
                                            request.targetDescriptorLifecycleGeneration(),
                                            request.targetOwner())) {
                                return completed(Optional.empty());
                            }
                            List<ZLinkStoreMutation> mutations = new ArrayList<>();
                            if (!ZLinkCapacityCounters.reserve(
                                    descriptor.orElseThrow(), bundle, conditions, mutations)) {
                                return completed(Optional.empty());
                            }
                            // A full target must refuse prepare before installing participant
                            // markers.
                            // The PREPARED write evaluates the same conditions and takes the
                            // reservation.
                            return capacityAvailable(conditions, cancellation)
                                    .thenApply(
                                            available ->
                                                    available
                                                            ? Optional.of(
                                                                    new AggregateCapacityReservation(
                                                                            conditions, mutations))
                                                            : Optional.empty());
                        });
    }

    private CompletionStage<ZLinkAggregatePrepareResult> refuseAggregatePrepare(
            ZLinkAggregateFence fence,
            ZLinkStoreReadFound staging,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            boolean ownsStaging) {
        CompletionStage<Boolean> cleanup =
                ownsStaging
                        ? abortOwnedAggregateStaging(
                                fence, staging.value().version().value(), cancellation)
                        : completed(false);
        return cleanup.thenApply(ignored -> new ZLinkAggregateConflict());
    }

    private static boolean isEmptyBundle(ZLinkPlacementCapacityBundle bundle) {
        return bundle.actorSlots() == 0 && bundle.spotSlots() == 0;
    }

    private CompletionStage<Boolean> abortOwnedAggregateStaging(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = aggregateKey(fence);
        return provider.read(key, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)
                                    || !found.value()
                                            .version()
                                            .value()
                                            .equals(expectedStoreVersion)) {
                                return completed(false);
                            }
                            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
                            if (aggregate.state() != AGGREGATE_STAGING) {
                                return completed(false);
                            }
                            return write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreVersionCondition(
                                                                    key, found.value().version())),
                                                    List.of(new ZLinkStoreDelete(key))),
                                            cancellation)
                                    .thenCompose(
                                            result -> {
                                                if (!(result instanceof ZLinkStoreWriteApplied)) {
                                                    return completed(false);
                                                }
                                                return clearAggregateMarkersByScan(
                                                                fence, cancellation)
                                                        .thenCompose(
                                                                cleaned ->
                                                                        cleaned
                                                                                ? aggregateInventory
                                                                                        .delete(
                                                                                                fence,
                                                                                                cancellation)
                                                                                        .thenApply(
                                                                                                ignored ->
                                                                                                        true)
                                                                                : completed(false));
                                            });
                        });
    }

    CompletionStage<ZLinkAggregateCommitResult> commitAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(cancellation, "cancellation");
        var completion = new CompletableFuture<ZLinkAggregateCommitResult>();
        completeAggregateCommit(fence, cancellation, completion);
        return completion;
    }

    private void completeAggregateCommit(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation,
            CompletableFuture<ZLinkAggregateCommitResult> completion) {
        CompletionStage<Optional<ZLinkAggregateCommitResult>> attempt;
        try {
            attempt = readAggregateCommitAttempt(fence, cancellation);
        } catch (RuntimeException failure) {
            completion.completeExceptionally(failure);
            return;
        }
        attempt.whenComplete(
                (terminal, failure) -> {
                    if (failure != null) {
                        completion.completeExceptionally(failure);
                    } else if (terminal.isPresent()) {
                        completion.complete(terminal.orElseThrow());
                    } else {
                        ForkJoinPool.commonPool()
                                .execute(
                                        () ->
                                                completeAggregateCommit(
                                                        fence, cancellation, completion));
                    }
                });
    }

    private CompletionStage<Optional<ZLinkAggregateCommitResult>> readAggregateCommitAttempt(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(Optional.of(ZLinkAggregateCommitResult.STALE));
                            }
                            PreparedAggregate prepared = decodeAggregate(found.value().bytes());
                            if (prepared.state() == AGGREGATE_COMMITTED) {
                                return aggregateInventory
                                        .load(
                                                fence,
                                                prepared.participantCount(),
                                                prepared.inventoryDigest(),
                                                opaqueCancellation)
                                        .thenCompose(
                                                participants ->
                                                        normalizeAggregateParticipants(
                                                                fence,
                                                                prepared.request(participants),
                                                                opaqueCancellation))
                                        .thenApply(
                                                ignored ->
                                                        Optional.of(
                                                                ZLinkAggregateCommitResult
                                                                        .ALREADY_COMMITTED));
                            }
                            if (prepared.state() != AGGREGATE_PREPARED) {
                                return completed(Optional.of(ZLinkAggregateCommitResult.STALE));
                            }
                            return aggregateInventory
                                    .load(
                                            fence,
                                            prepared.participantCount(),
                                            prepared.inventoryDigest(),
                                            opaqueCancellation)
                                    .thenCompose(
                                            participants -> {
                                                ZLinkAggregatePrepareRequest request =
                                                        prepared.request(participants);
                                                return loadParticipants(
                                                                request.participants(),
                                                                opaqueCancellation)
                                                        .thenCompose(
                                                                rows -> {
                                                                    if (rows.size()
                                                                                    != request.participants()
                                                                                            .size()
                                                                            || !aggregateMarkersMatch(
                                                                                    request, fence,
                                                                                    rows)) {
                                                                        return completed(
                                                                                Optional.of(
                                                                                        ZLinkAggregateCommitResult
                                                                                                .STALE));
                                                                    }
                                                                    List<ZLinkStoreCondition>
                                                                            conditions =
                                                                                    new ArrayList<>();
                                                                    conditions.add(
                                                                            new ZLinkStoreVersionCondition(
                                                                                    marker,
                                                                                    found.value()
                                                                                            .version()));
                                                                    return requireLiveOwner(
                                                                                    request
                                                                                            .targetOwner(),
                                                                                    conditions,
                                                                                    opaqueCancellation)
                                                                            .thenCompose(
                                                                                    live ->
                                                                                            live
                                                                                                    ? requireAggregateMembership(
                                                                                                            request,
                                                                                                            rows,
                                                                                                            conditions,
                                                                                                            opaqueCancellation)
                                                                                                    : completed(
                                                                                                            false))
                                                                            .thenCompose(
                                                                                    live -> {
                                                                                        if (!live) {
                                                                                            return completed(
                                                                                                    Optional
                                                                                                            .of(
                                                                                                                    ZLinkAggregateCommitResult
                                                                                                                            .STALE));
                                                                                        }
                                                                                        var
                                                                                                mutations =
                                                                                                        aggregateCapacityMutations(
                                                                                                                request,
                                                                                                                rows);
                                                                                        mutations
                                                                                                .add(
                                                                                                        new ZLinkStorePut(
                                                                                                                marker,
                                                                                                                encodeAggregate(
                                                                                                                        AGGREGATE_COMMITTED,
                                                                                                                        request),
                                                                                                                null));
                                                                                        return write(
                                                                                                        new ZLinkStoreWriteRequest(
                                                                                                                conditions,
                                                                                                                mutations),
                                                                                                        opaqueCancellation)
                                                                                                .thenCompose(
                                                                                                        result -> {
                                                                                                            if (!(result
                                                                                                                    instanceof
                                                                                                                    ZLinkStoreWriteApplied)) {
                                                                                                                return retryAggregateCommit(
                                                                                                                        fence,
                                                                                                                        cancellation,
                                                                                                                        found.value()
                                                                                                                                .version(),
                                                                                                                        request);
                                                                                                            }
                                                                                                            return normalizeAggregateParticipants(
                                                                                                                            fence,
                                                                                                                            request,
                                                                                                                            opaqueCancellation)
                                                                                                                    .thenApply(
                                                                                                                            ignored ->
                                                                                                                                    Optional
                                                                                                                                            .of(
                                                                                                                                                    ZLinkAggregateCommitResult
                                                                                                                                                            .COMMITTED));
                                                                                                        });
                                                                                    });
                                                                });
                                            });
                        });
    }

    private List<ZLinkStoreMutation> aggregateCapacityMutations(
            ZLinkAggregatePrepareRequest request, List<LoadedParticipant> rows) {
        var deltas = new ZLinkCapacityCounters();
        var target = request.targetDescriptor();
        deltas.change(target, request.capacityBundle(), -1, 0);
        for (int index = 0; index < rows.size(); index++) {
            if (request.participants().get(index).ownerTransition()
                    != ZLinkAuthorityGenerationTransition.NEW_OWNER) continue;
            var allocation = decode(rows.get(index).value().bytes()).allocation();
            deltas.change(allocation.descriptor(), allocation.capacityBundle(), 0, -1);
            deltas.change(target, allocation.capacityBundle(), 0, 1);
        }
        List<ZLinkStoreMutation> mutations = new ArrayList<>();
        deltas.appendTo(mutations);
        return mutations;
    }

    private CompletionStage<Optional<ZLinkAggregateCommitResult>> retryAggregateCommit(
            ZLinkAggregateFence fence,
            ZLinkStoreCancellation cancellation,
            ZLinkStoreVersion initialVersion,
            ZLinkAggregatePrepareRequest request) {
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(Optional.of(ZLinkAggregateCommitResult.STALE));
                            }
                            PreparedAggregate current = decodeAggregate(found.value().bytes());
                            if (!sameAggregateRequest(current, request)) {
                                return completed(Optional.of(ZLinkAggregateCommitResult.STALE));
                            }
                            if (current.state() == AGGREGATE_COMMITTED) {
                                return aggregateInventory
                                        .load(
                                                fence,
                                                current.participantCount(),
                                                current.inventoryDigest(),
                                                opaqueCancellation)
                                        .thenCompose(
                                                participants ->
                                                        normalizeAggregateParticipants(
                                                                fence,
                                                                current.request(participants),
                                                                opaqueCancellation))
                                        .thenApply(
                                                ignored ->
                                                        Optional.of(
                                                                ZLinkAggregateCommitResult
                                                                        .ALREADY_COMMITTED));
                            }
                            if (current.state() != AGGREGATE_PREPARED
                                    || !found.value().version().equals(initialVersion)
                                    || cancellation.isCancellationRequested()) {
                                return completed(Optional.of(ZLinkAggregateCommitResult.STALE));
                            }
                            return requireLiveOwner(
                                            request.targetOwner(),
                                            new ArrayList<>(),
                                            opaqueCancellation)
                                    .thenCompose(
                                            live ->
                                                    live
                                                            ? completed(Optional.empty())
                                                            : completed(
                                                                    Optional.of(
                                                                            ZLinkAggregateCommitResult
                                                                                    .STALE)));
                        });
    }

    CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        return provider.read(aggregateKey(fence), adapt(cancellation))
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(Optional.empty());
                            }
                            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
                            if (aggregate.state() != AGGREGATE_COMMITTED) {
                                return completed(Optional.empty());
                            }
                            return aggregateInventory
                                    .load(
                                            fence,
                                            aggregate.participantCount(),
                                            aggregate.inventoryDigest(),
                                            adapt(cancellation))
                                    .thenApply(
                                            participants ->
                                                    Optional.of(
                                                            new ZLinkAggregateProgressSnapshot(
                                                                    fence,
                                                                    found.value().version().value(),
                                                                    aggregate.request(
                                                                            participants))));
                        });
    }

    CompletionStage<Boolean> removeAggregateProgress(
            ZLinkAggregateFence fence,
            String expectedStoreVersion,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(expectedStoreVersion, "expectedStoreVersion");
        ZLinkStoreKey marker = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(marker, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return aggregateInventory
                                        .delete(fence, opaqueCancellation)
                                        .thenApply(ignored -> true);
                            }
                            if (!found.value().version().value().equals(expectedStoreVersion)) {
                                return completed(false);
                            }
                            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
                            if (aggregate.state() != AGGREGATE_COMMITTED) {
                                return completed(false);
                            }
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            conditions.add(
                                    new ZLinkStoreVersionCondition(
                                            marker, found.value().version()));
                            return requireLiveOwner(
                                            aggregate.targetOwner(), conditions, opaqueCancellation)
                                    .thenCompose(
                                            live ->
                                                    !live
                                                            ? completed(false)
                                                            : write(
                                                                            new ZLinkStoreWriteRequest(
                                                                                    conditions,
                                                                                    List.of(
                                                                                            new ZLinkStoreDelete(
                                                                                                    marker))),
                                                                            opaqueCancellation)
                                                                    .thenCompose(
                                                                            result ->
                                                                                    result
                                                                                                    instanceof
                                                                                                    ZLinkStoreWriteApplied
                                                                                            ? aggregateInventory
                                                                                                    .delete(
                                                                                                            fence,
                                                                                                            opaqueCancellation)
                                                                                                    .thenApply(
                                                                                                            ignored ->
                                                                                                                    true)
                                                                                            : completed(
                                                                                                    false)));
                        });
    }

    CompletionStage<Void> settleLegacyAggregates(
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return scanLegacyAggregates(null, new ArrayList<>(), cancellation)
                .thenCompose(
                        items -> {
                            // Validate leases and capacity before changing any legacy row.
                            var projected = new HashMap<ZLinkStoreKey, ZLinkLegacyCapacity>();
                            CompletionStage<Void> validated = completed(null);
                            for (var item : items) {
                                validated =
                                        validated.thenCompose(
                                                ignored ->
                                                        validateLegacyAggregate(
                                                                item, projected, cancellation));
                            }
                            return validated.thenCompose(
                                    ignored -> {
                                        CompletionStage<Void> settled = completed(null);
                                        for (var item : items) {
                                            settled =
                                                    settled.thenCompose(
                                                            done ->
                                                                    settleLegacyAggregate(
                                                                            item, cancellation));
                                        }
                                        return settled;
                                    });
                        });
    }

    private CompletionStage<Void> validateLegacyAggregate(
            ZLinkStoreScanItem item,
            Map<ZLinkStoreKey, ZLinkLegacyCapacity> projected,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        var aggregate = legacyAggregate(item);
        var owner = aggregate.targetOwner();
        return provider.read(ownerKey(owner.ownerId()), cancellation)
                .thenCompose(
                        lease -> {
                            if (ZLinkProviderOwnerLeaseRepository.isLive(
                                    lease, owner.ownerId(), owner.leaseGeneration())) {
                                throw new ZLinkConfigurationException(
                                        "v11 aggregate has a live target owner: "
                                                + item.key().value());
                            }
                            return planLegacyRelease(item, aggregate, projected, cancellation)
                                    .thenApply(release -> null);
                        });
    }

    private CompletionStage<List<ZLinkStoreScanItem>> scanLegacyAggregates(
            ZLinkStoreScanCursor cursor,
            List<ZLinkStoreScanItem> items,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return provider.scan(
                        new ZLinkStoreScanRequest(AGGREGATE_PREFIX, cursor, 1000), cancellation)
                .thenComposeAsync(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired)
                                return scanLegacyAggregates(null, new ArrayList<>(), cancellation);
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            items.addAll(page.items());
                            return page.nextCursor() == null
                                    ? completed(items)
                                    : scanLegacyAggregates(page.nextCursor(), items, cancellation);
                        });
    }

    private static PreparedAggregate legacyAggregate(ZLinkStoreScanItem item) {
        try {
            PreparedAggregate aggregate = decodeAggregate(item.value().bytes());
            // A committed aggregate already moved its authority rows and v11 capacity; settling it
            // only clears its markers, inventory and root, like the other states.
            if (!aggregateKey(
                            new ZLinkAggregateFence(
                                    aggregate.aggregateId(), aggregate.aggregateGeneration()))
                    .equals(item.key())) {
                throw new IllegalStateException("v11 aggregate key does not match its record");
            }
            aggregate.capacityBundle();
            return aggregate;
        } catch (IllegalArgumentException | IllegalStateException failure) {
            throw new ZLinkConfigurationException(
                    "Cannot settle v11 aggregate " + item.key().value(), failure);
        }
    }

    private CompletionStage<Void> settleLegacyAggregate(
            ZLinkStoreScanItem item,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        PreparedAggregate aggregate = legacyAggregate(item);
        var fence =
                new ZLinkAggregateFence(aggregate.aggregateId(), aggregate.aggregateGeneration());
        var conditions = new ArrayList<ZLinkStoreCondition>();
        conditions.add(new ZLinkStoreVersionCondition(item.key(), item.value().version()));
        var mutations = new ArrayList<ZLinkStoreMutation>();
        mutations.add(new ZLinkStoreDelete(item.key()));

        return planLegacyRelease(item, aggregate, null, cancellation)
                .thenCompose(
                        release -> {
                            if (release != null) {
                                conditions.add(
                                        new ZLinkStoreVersionCondition(
                                                release.key(), release.version()));
                                mutations.add(
                                        new ZLinkStorePut(release.key(), release.bytes(), null));
                            }
                            // Leave the root discoverable until marker and inventory cleanup can be
                            // resumed safely.
                            return clearAggregateMarkersByScan(fence, cancellation)
                                    .thenCompose(
                                            cleared -> {
                                                if (!cleared)
                                                    return retryLegacyAggregate(
                                                            item.key(), cancellation);
                                                return aggregateInventory
                                                        .delete(fence, cancellation)
                                                        .thenCompose(
                                                                ignored ->
                                                                        write(
                                                                                new ZLinkStoreWriteRequest(
                                                                                        conditions,
                                                                                        mutations),
                                                                                cancellation))
                                                        .thenCompose(
                                                                result ->
                                                                        result
                                                                                        instanceof
                                                                                        ZLinkStoreWriteApplied
                                                                                ? completed(null)
                                                                                : retryLegacyAggregate(
                                                                                        item.key(),
                                                                                        cancellation));
                                            });
                        });
    }

    private CompletionStage<LegacyCapacityRelease> planLegacyRelease(
            ZLinkStoreScanItem item,
            PreparedAggregate aggregate,
            Map<ZLinkStoreKey, ZLinkLegacyCapacity> projected,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        var bundle = aggregate.capacityBundle();
        if (aggregate.state() != AGGREGATE_PREPARED || isEmptyBundle(bundle))
            return completed(null);
        var key = ZLinkLegacyCapacity.key(aggregate.targetDescriptor());
        return provider.read(key, cancellation)
                .thenCompose(
                        read ->
                                provider.read(item.key(), cancellation)
                                        .thenApply(
                                                current -> {
                                                    if (!(current
                                                            instanceof ZLinkStoreReadFound root))
                                                        return null;
                                                    if (!root.value()
                                                            .version()
                                                            .equals(item.value().version()))
                                                        throw new ZLinkConfigurationException(
                                                                "v11 aggregate changed during conversion: "
                                                                        + item.key().value());
                                                    if (!(read
                                                            instanceof
                                                            ZLinkStoreReadFound capacity))
                                                        throw new ZLinkConfigurationException(
                                                                "Missing v11 capacity for aggregate "
                                                                        + item.key().value());
                                                    var row =
                                                            projected == null
                                                                    ? ZLinkLegacyCapacity.decode(
                                                                            key,
                                                                            capacity.value()
                                                                                    .bytes())
                                                                    : projected.computeIfAbsent(
                                                                            key,
                                                                            ignored ->
                                                                                    ZLinkLegacyCapacity
                                                                                            .decode(
                                                                                                    key,
                                                                                                    capacity.value()
                                                                                                            .bytes()));
                                                    return new LegacyCapacityRelease(
                                                            key,
                                                            capacity.value().version(),
                                                            row.release(bundle));
                                                }));
    }

    private record LegacyCapacityRelease(
            ZLinkStoreKey key, ZLinkStoreVersion version, byte[] bytes) {}

    private CompletionStage<Void> retryLegacyAggregate(
            ZLinkStoreKey key,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return provider.read(key, cancellation)
                .thenComposeAsync(
                        read ->
                                read instanceof ZLinkStoreReadFound found
                                        ? settleLegacyAggregate(
                                                new ZLinkStoreScanItem(key, found.value()),
                                                cancellation)
                                        : completed(null));
    }

    CompletionStage<ZLinkAggregateAbortResult> abortAggregate(
            ZLinkAggregateFence fence, ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = aggregateKey(fence);
        var opaqueCancellation = adapt(cancellation);
        return provider.read(key, opaqueCancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return clearAggregateMarkersByScan(fence, opaqueCancellation)
                                        .thenCompose(
                                                cleaned ->
                                                        cleaned
                                                                ? aggregateInventory
                                                                        .delete(
                                                                                fence,
                                                                                opaqueCancellation)
                                                                        .thenApply(
                                                                                ignored ->
                                                                                        ZLinkAggregateAbortResult
                                                                                                .ALREADY_ABORTED)
                                                                : completed(
                                                                        ZLinkAggregateAbortResult
                                                                                .STALE));
                            }
                            PreparedAggregate aggregate = decodeAggregate(found.value().bytes());
                            if (aggregate.state() == AGGREGATE_COMMITTED) {
                                return completed(ZLinkAggregateAbortResult.STALE);
                            }
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            conditions.add(
                                    new ZLinkStoreVersionCondition(key, found.value().version()));
                            List<ZLinkStoreMutation> mutations = new ArrayList<>();
                            mutations.add(new ZLinkStoreDelete(key));
                            if (aggregate.state() == AGGREGATE_PREPARED) {
                                var deltas = new ZLinkCapacityCounters();
                                deltas.change(
                                        aggregate.targetDescriptor(),
                                        aggregate.capacityBundle(),
                                        -1,
                                        0);
                                deltas.appendTo(mutations);
                            }
                            return write(
                                            new ZLinkStoreWriteRequest(conditions, mutations),
                                            opaqueCancellation)
                                    .thenCompose(
                                            result -> {
                                                if (!(result instanceof ZLinkStoreWriteApplied)) {
                                                    // Reconstruct the abort from the current
                                                    // marker.
                                                    return cancellation.isCancellationRequested()
                                                            ? completed(
                                                                    ZLinkAggregateAbortResult.STALE)
                                                            : abortAggregate(fence, cancellation);
                                                }
                                                return clearAggregateMarkersByScan(
                                                                fence, opaqueCancellation)
                                                        .thenCompose(
                                                                cleaned ->
                                                                        cleaned
                                                                                ? aggregateInventory
                                                                                        .delete(
                                                                                                fence,
                                                                                                opaqueCancellation)
                                                                                        .thenApply(
                                                                                                ignored ->
                                                                                                        ZLinkAggregateAbortResult
                                                                                                                .ABORTED)
                                                                                : completed(
                                                                                        ZLinkAggregateAbortResult
                                                                                                .STALE));
                                            });
                        });
    }

    CompletionStage<Long> removeAllByOwner(ZLinkLocationOwnerToken owner) {
        Objects.requireNonNull(owner, "owner");
        return removeAllByOwner(owner, () -> false);
    }

    CompletionStage<Long> removeAllByOwner(
            ZLinkLocationOwnerToken owner,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return removeAllByOwner(owner, null, 0L, 0, cancellation);
    }

    private CompletionStage<Long> removeAllByOwner(
            ZLinkLocationOwnerToken owner,
            ZLinkStoreScanCursor cursor,
            long removed,
            int restartCount,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested())
            return failed(
                    new java.util.concurrent.TimeoutException("host shutdown deadline expired"));
        return provider.scan(
                        new ZLinkStoreScanRequest(AUTHORITY_PREIMAGE_PREFIX, cursor, 1000),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                if (restartCount >= 8) {
                                    return failed(
                                            new IllegalStateException(
                                                    "authority cleanup scan repeatedly expired"));
                                }
                                return removeAllByOwner(
                                        owner, null, removed, restartCount + 1, cancellation);
                            }
                            ZLinkStoreScanPage page = ((ZLinkStoreScanPageResult) result).value();
                            CompletionStage<Long> cleanup = completed(removed);
                            for (ZLinkStoreScanItem item : page.items()) {
                                AuthorityRecord record = decode(item.value().bytes());
                                if (record.aggregate() != null
                                        || !record.ownerId().equals(owner.ownerId())
                                        || record.ownerLeaseGeneration()
                                                != owner.leaseGeneration()) {
                                    continue;
                                }
                                String key = decodeAuthorityKey(item.key());
                                if (record.allocation().state()
                                        == ZLinkPlacementAllocationState.ACTIVE) {
                                    cleanup =
                                            cleanup.thenCompose(
                                                    count ->
                                                            compareExchange(
                                                                            key,
                                                                            new ZLinkAuthorityExpectFound(
                                                                                    item.value()
                                                                                            .version()
                                                                                            .value()),
                                                                            new ZLinkAuthorityDelete(),
                                                                            cancellation
                                                                                    ::isCancellationRequested)
                                                                    .thenApply(
                                                                            write ->
                                                                                    write
                                                                                                    instanceof
                                                                                                    ZLinkAuthorityDeleted
                                                                                            ? Math
                                                                                                    .addExact(
                                                                                                            count,
                                                                                                            1L)
                                                                                            : count));
                                } else if (record.pendingCreation().isPresent()) {
                                    var reservation =
                                            new ZLinkObjectReservation(
                                                    key,
                                                    item.value().version().value(),
                                                    record.objectGeneration(),
                                                    record.authorityOwnerGeneration(),
                                                    record.pendingCreation().get().reservationId(),
                                                    record.allocation().descriptor(),
                                                    record.allocation()
                                                            .descriptorLifecycleGeneration(),
                                                    owner);
                                    cleanup =
                                            cleanup.thenCompose(
                                                    count ->
                                                            abort(
                                                                            reservation,
                                                                            cancellation
                                                                                    ::isCancellationRequested)
                                                                    .thenCompose(
                                                                            write ->
                                                                                    write
                                                                                                    == ZLinkObjectAbortResult
                                                                                                            .ABORTED
                                                                                            ? completed(
                                                                                                    true)
                                                                                            : write
                                                                                                            == ZLinkObjectAbortResult
                                                                                                                    .STALE
                                                                                                    ? releaseEndedReservation(
                                                                                                            key,
                                                                                                            item.value()
                                                                                                                    .version()
                                                                                                                    .value(),
                                                                                                            cancellation,
                                                                                                            null,
                                                                                                            owner,
                                                                                                            null,
                                                                                                            null)
                                                                                                    : completed(
                                                                                                            false))
                                                                    .thenApply(
                                                                            released ->
                                                                                    released
                                                                                            ? Math
                                                                                                    .addExact(
                                                                                                            count,
                                                                                                            1L)
                                                                                            : count));
                                }
                            }
                            return cleanup.thenCompose(
                                    count ->
                                            continueOwnerCleanupScan(
                                                    owner,
                                                    page.nextCursor(),
                                                    count,
                                                    restartCount,
                                                    cancellation));
                        });
    }

    private CompletionStage<Long> continueOwnerCleanupScan(
            ZLinkLocationOwnerToken owner,
            ZLinkStoreScanCursor cursor,
            long removed,
            int restartCount,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return cursor == null
                ? completed(removed)
                : removeAllByOwner(owner, cursor, removed, restartCount, cancellation);
    }

    private CompletionStage<List<LoadedParticipant>> loadParticipants(
            List<ZLinkAggregateParticipant> participants,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        CompletionStage<List<LoadedParticipant>> loaded = completed(new ArrayList<>());
        for (ZLinkAggregateParticipant participant : participants) {
            loaded =
                    loaded.thenCompose(
                            rows ->
                                    provider.read(
                                                    authorityKey(participant.authorityKey()),
                                                    cancellation)
                                            .thenApply(
                                                    read -> {
                                                        if (read
                                                                instanceof
                                                                ZLinkStoreReadFound found) {
                                                            rows.add(
                                                                    new LoadedParticipant(
                                                                            authorityKey(
                                                                                    participant
                                                                                            .authorityKey()),
                                                                            found.value()));
                                                        }
                                                        return rows;
                                                    }));
        }
        return loaded.thenApply(List::copyOf);
    }

    private static boolean aggregateMarkersMatch(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            List<LoadedParticipant> rows) {
        for (int index = 0; index < rows.size(); index++) {
            ZLinkAggregateParticipant participant = request.participants().get(index);
            AuthorityRecord record = decode(rows.get(index).value().bytes());
            AggregateParticipantMarker marker = record.aggregate();
            if (marker == null
                    || !marker.aggregateId().equals(fence.aggregateId())
                    || marker.aggregateGeneration() != fence.aggregateGeneration()
                    || marker.index() != index
                    || !marker.expectedStoreVersion().equals(participant.expectedStoreVersion())
                    || marker.ownerTransition() != participant.ownerTransition()
                    || !Arrays.equals(
                            marker.authorityPayloadSha256(),
                            ZLinkAggregateInventoryStore.sha256(participant.authorityPayload()))
                    || !Arrays.equals(
                            marker.membershipMutationSha256(),
                            ZLinkAggregateInventoryStore.sha256(
                                    participant.membershipMutation()))) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameAggregateMarker(
            AggregateParticipantMarker marker,
            ZLinkAggregateFence fence,
            int index,
            ZLinkAggregateParticipant participant) {
        return marker != null
                && marker.aggregateId().equals(fence.aggregateId())
                && marker.aggregateGeneration() == fence.aggregateGeneration()
                && marker.index() == index
                && marker.expectedStoreVersion().equals(participant.expectedStoreVersion())
                && marker.ownerTransition() == participant.ownerTransition()
                && Arrays.equals(
                        marker.authorityPayloadSha256(),
                        ZLinkAggregateInventoryStore.sha256(participant.authorityPayload()))
                && Arrays.equals(
                        marker.membershipMutationSha256(),
                        ZLinkAggregateInventoryStore.sha256(participant.membershipMutation()));
    }

    private CompletionStage<Void> normalizeAggregateParticipants(
            ZLinkAggregateFence fence,
            ZLinkAggregatePrepareRequest request,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (int index = 0; index < request.participants().size(); index++) {
            int participantIndex = index;
            chain =
                    chain.thenCompose(
                            ignored ->
                                    normalizeAggregateParticipant(
                                            fence, request, participantIndex, cancellation, 0));
        }
        return chain;
    }

    private CompletionStage<Void> normalizeAggregateParticipant(
            ZLinkAggregateFence fence,
            ZLinkAggregatePrepareRequest request,
            int index,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            int retry) {
        ZLinkAggregateParticipant participant = request.participants().get(index);
        ZLinkStoreKey rowKey = authorityKey(participant.authorityKey());
        return provider.read(rowKey, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return failed(
                                        new IllegalStateException(
                                                "aggregate participant authority is missing during"
                                                        + " normalization"));
                            }
                            AuthorityRecord current = decode(found.value().bytes());
                            AggregateParticipantMarker marker = current.aggregate();
                            if (marker == null) {
                                return aggregateInventory
                                        .readParticipantPayload(fence, index, cancellation)
                                        .thenCompose(
                                                payload ->
                                                        isNormalizedAggregateParticipant(
                                                                        current,
                                                                        participant,
                                                                        request,
                                                                        payload)
                                                                ? completed(null)
                                                                : failed(
                                                                        new IllegalStateException(
                                                                                "aggregate"
                                                                                        + " participant"
                                                                                        + " was normalized"
                                                                                        + " to an"
                                                                                        + " unexpected"
                                                                                        + " state")));
                            }
                            if (!marker.aggregateId().equals(fence.aggregateId())
                                    || marker.aggregateGeneration() != fence.aggregateGeneration()
                                    || marker.index() != index
                                    || !marker.expectedStoreVersion()
                                            .equals(participant.expectedStoreVersion())
                                    || marker.ownerTransition() != participant.ownerTransition()) {
                                return failed(
                                        new IllegalStateException(
                                                "aggregate participant fence changed during"
                                                        + " normalization"));
                            }
                            return aggregateInventory
                                    .readParticipantPayload(fence, index, cancellation)
                                    .thenCompose(
                                            payload -> {
                                                if (!Arrays.equals(
                                                        marker.authorityPayloadSha256(),
                                                        ZLinkAggregateInventoryStore.sha256(
                                                                payload))) {
                                                    return failed(
                                                            new IllegalStateException(
                                                                    "aggregate participant payload"
                                                                            + " checksum is invalid"));
                                                }
                                                boolean moves =
                                                        marker.ownerTransition()
                                                                == ZLinkAuthorityGenerationTransition
                                                                        .NEW_OWNER;
                                                ZLinkPlacementAllocation allocation =
                                                        moves
                                                                ? new ZLinkPlacementAllocation(
                                                                        ZLinkPlacementAllocationState
                                                                                .ACTIVE,
                                                                        current.allocation()
                                                                                .objectKind(),
                                                                        current.allocation()
                                                                                .stableType(),
                                                                        request.targetDescriptor(),
                                                                        request
                                                                                .targetDescriptorLifecycleGeneration(),
                                                                        current.allocation()
                                                                                .capacityBundle())
                                                                : current.allocation();
                                                AuthorityRecord next =
                                                        new AuthorityRecord(
                                                                        payload,
                                                                        current.objectGeneration(),
                                                                        moves
                                                                                ? marker
                                                                                        .targetAuthorityOwnerGeneration()
                                                                                : current
                                                                                        .authorityOwnerGeneration(),
                                                                        moves
                                                                                ? request.targetOwner()
                                                                                        .ownerId()
                                                                                : current.ownerId(),
                                                                        moves
                                                                                ? request.targetOwner()
                                                                                        .leaseGeneration()
                                                                                : current
                                                                                        .ownerLeaseGeneration(),
                                                                        allocation,
                                                                        Optional.empty())
                                                                .withCommittedRelocation(
                                                                        current.relocation());
                                                return write(
                                                                new ZLinkStoreWriteRequest(
                                                                        List.of(
                                                                                new ZLinkStoreVersionCondition(
                                                                                        rowKey,
                                                                                        found.value()
                                                                                                .version())),
                                                                        List.of(
                                                                                new ZLinkStorePut(
                                                                                        rowKey,
                                                                                        encode(
                                                                                                next),
                                                                                        null))),
                                                                cancellation)
                                                        .thenCompose(
                                                                result ->
                                                                        result
                                                                                        instanceof
                                                                                        ZLinkStoreWriteApplied
                                                                                ? completed(null)
                                                                                : retry >= 8
                                                                                        ? failed(
                                                                                                new IllegalStateException(
                                                                                                        "aggregate"
                                                                                                                + " participant"
                                                                                                                + " normalization"
                                                                                                                + " repeatedly"
                                                                                                                + " conflicted"))
                                                                                        : normalizeAggregateParticipant(
                                                                                                fence,
                                                                                                request,
                                                                                                index,
                                                                                                cancellation,
                                                                                                retry
                                                                                                        + 1));
                                            });
                        });
    }

    private static boolean isNormalizedAggregateParticipant(
            AuthorityRecord current,
            ZLinkAggregateParticipant participant,
            ZLinkAggregatePrepareRequest request,
            byte[] payload) {
        if (!Arrays.equals(current.payload(), payload)
                || current.objectGeneration() != participant.objectGeneration()
                || current.allocation().state() != ZLinkPlacementAllocationState.ACTIVE
                || current.visibleStoreVersion() != null) {
            return false;
        }
        if (participant.ownerTransition() == ZLinkAuthorityGenerationTransition.NEW_OWNER) {
            return current.authorityOwnerGeneration() > participant.sourceAuthorityOwnerGeneration()
                    && current.ownerId().equals(request.targetOwner().ownerId())
                    && current.ownerLeaseGeneration() == request.targetOwner().leaseGeneration()
                    && current.allocation().descriptor().equals(request.targetDescriptor())
                    && current.allocation().descriptorLifecycleGeneration()
                            == request.targetDescriptorLifecycleGeneration();
        }
        return current.authorityOwnerGeneration() == participant.sourceAuthorityOwnerGeneration();
    }

    private CompletionStage<AggregateMarkerInstallation> installAggregateMarkers(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            Instant retryDeadline) {
        CompletionStage<AggregateMarkerInstallation> chain =
                completed(AggregateMarkerInstallation.installedResult());
        List<ZLinkStoreKey> installed = new ArrayList<>();
        for (int index = 0; index < request.participants().size(); index++) {
            int participantIndex = index;
            chain =
                    chain.thenCompose(
                            result ->
                                    result.installed()
                                            ? installAggregateMarker(
                                                    request,
                                                    fence,
                                                    participantIndex,
                                                    installed,
                                                    cancellation,
                                                    retryDeadline)
                                            : completed(result));
        }
        return chain.thenCompose(
                result ->
                        result.installed()
                                ? completed(result)
                                : clearAggregateMarkers(fence, installed, cancellation)
                                        .thenApply(ignored -> result));
    }

    private CompletionStage<AggregateMarkerInstallation> installAggregateMarker(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            int index,
            List<ZLinkStoreKey> installed,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            Instant retryDeadline) {
        var participant = request.participants().get(index);
        var rowKey = authorityKey(participant.authorityKey());
        return provider.read(rowKey, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)) {
                                return completed(AggregateMarkerInstallation.conflictResult());
                            }
                            var current = decode(found.value().bytes());
                            boolean newOwner =
                                    participant.ownerTransition()
                                            == ZLinkAuthorityGenerationTransition.NEW_OWNER;
                            var owner =
                                    newOwner
                                            ? request.targetOwner()
                                            : new ZLinkLocationOwnerToken(
                                                    current.ownerId(),
                                                    current.ownerLeaseGeneration());
                            List<ZLinkStoreCondition> conditions = new ArrayList<>();
                            return requireLiveOwner(owner, conditions, cancellation)
                                    .thenCompose(
                                            live -> {
                                                if (!live)
                                                    return completed(
                                                            AggregateMarkerInstallation
                                                                    .conflictResult());
                                                if (current.aggregate() != null) {
                                                    return completed(
                                                            sameAggregateMarker(
                                                                            current.aggregate(),
                                                                            fence,
                                                                            index,
                                                                            participant)
                                                                    ? AggregateMarkerInstallation
                                                                            .installedResult()
                                                                    : AggregateMarkerInstallation
                                                                            .conflictResult());
                                                }
                                                if (!found.value()
                                                                .version()
                                                                .value()
                                                                .equals(
                                                                        participant
                                                                                .expectedStoreVersion())
                                                        || current.allocation().state()
                                                                != ZLinkPlacementAllocationState
                                                                        .ACTIVE) {
                                                    return completed(
                                                            AggregateMarkerInstallation
                                                                    .conflictResult());
                                                }
                                                CompletionStage<Counter> generation =
                                                        newOwner
                                                                ? nextCounter(
                                                                        generationKey(
                                                                                "authority-owner-counter",
                                                                                participant
                                                                                        .authorityKey()),
                                                                        conditions,
                                                                        cancellation)
                                                                : completed(
                                                                        new Counter(
                                                                                current
                                                                                        .authorityOwnerGeneration(),
                                                                                List.of(),
                                                                                false));
                                                return generation.thenCompose(
                                                        counter -> {
                                                            if (counter.exhausted())
                                                                return completed(
                                                                        AggregateMarkerInstallation
                                                                                .exhaustedResult());
                                                            var marker =
                                                                    new AggregateParticipantMarker(
                                                                            fence.aggregateId(),
                                                                            fence
                                                                                    .aggregateGeneration(),
                                                                            index,
                                                                            participant
                                                                                    .expectedStoreVersion(),
                                                                            participant
                                                                                    .ownerTransition(),
                                                                            counter.value(),
                                                                            ZLinkAggregateInventoryStore
                                                                                    .sha256(
                                                                                            participant
                                                                                                    .authorityPayload()),
                                                                            ZLinkAggregateInventoryStore
                                                                                    .sha256(
                                                                                            participant
                                                                                                    .membershipMutation()));
                                                            var visibleVersion =
                                                                    current.visibleStoreVersion()
                                                                                    == null
                                                                            ? found.value()
                                                                                    .version()
                                                                                    .value()
                                                                            : current
                                                                                    .visibleStoreVersion();
                                                            var marked =
                                                                    current.withAggregate(
                                                                                    marker,
                                                                                    visibleVersion)
                                                                            .withRelocation(
                                                                                    participant
                                                                                                            .relocation()
                                                                                                    == null
                                                                                            ? current
                                                                                                    .relocation()
                                                                                            : participant
                                                                                                    .relocation());
                                                            conditions.add(
                                                                    new ZLinkStoreVersionCondition(
                                                                            rowKey,
                                                                            found.value()
                                                                                    .version()));
                                                            List<ZLinkStoreMutation> mutations =
                                                                    new ArrayList<>(
                                                                            counter.mutations());
                                                            mutations.add(
                                                                    new ZLinkStorePut(
                                                                            rowKey,
                                                                            encode(marked),
                                                                            null));
                                                            return write(
                                                                            new ZLinkStoreWriteRequest(
                                                                                    conditions,
                                                                                    mutations),
                                                                            cancellation)
                                                                    .thenCompose(
                                                                            result -> {
                                                                                if (result
                                                                                        instanceof
                                                                                        ZLinkStoreWriteApplied) {
                                                                                    installed.add(
                                                                                            rowKey);
                                                                                    return completed(
                                                                                            AggregateMarkerInstallation
                                                                                                    .installedResult());
                                                                                }
                                                                                return resolveAggregateMarkerRace(
                                                                                        request,
                                                                                        fence,
                                                                                        index,
                                                                                        rowKey,
                                                                                        cancellation,
                                                                                        retryDeadline);
                                                                            });
                                                        });
                                            });
                        });
    }

    private CompletionStage<AggregateMarkerInstallation> resolveAggregateMarkerRace(
            ZLinkAggregatePrepareRequest request,
            ZLinkAggregateFence fence,
            int index,
            ZLinkStoreKey rowKey,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            Instant retryDeadline) {
        return provider.read(rowKey, cancellation)
                .thenCompose(
                        raced -> {
                            if (!(raced instanceof ZLinkStoreReadFound found)
                                    || !sameAggregateMarker(
                                            decode(found.value().bytes()).aggregate(),
                                            fence,
                                            index,
                                            request.participants().get(index))) {
                                return completed(AggregateMarkerInstallation.conflictResult());
                            }
                            return provider.read(aggregateKey(fence), cancellation)
                                    .thenCompose(
                                            aggregateRead -> {
                                                if (!(aggregateRead
                                                        instanceof
                                                        ZLinkStoreReadFound aggregateFound)) {
                                                    return completed(
                                                            AggregateMarkerInstallation
                                                                    .conflictResult());
                                                }
                                                var aggregate =
                                                        decodeAggregate(
                                                                aggregateFound.value().bytes());
                                                if (!sameAggregateRequest(aggregate, request)
                                                        || (aggregate.state() != AGGREGATE_STAGING
                                                                && aggregate.state()
                                                                        != AGGREGATE_PREPARED)) {
                                                    return completed(
                                                            AggregateMarkerInstallation
                                                                    .conflictResult());
                                                }
                                                return prepareAggregate(
                                                                request,
                                                                cancellation
                                                                        ::isCancellationRequested,
                                                                retryDeadline)
                                                        .thenApply(
                                                                AggregateMarkerInstallation
                                                                        ::reenteredResult);
                                            });
                        });
    }

    private CompletionStage<Boolean> clearAggregateMarkers(
            ZLinkAggregateFence fence,
            List<ZLinkStoreKey> installed,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        CompletionStage<Boolean> chain = completed(true);
        for (ZLinkStoreKey rowKey : installed) {
            chain =
                    chain.thenCompose(
                            ok ->
                                    provider.read(rowKey, cancellation)
                                            .thenCompose(
                                                    read -> {
                                                        if (!(read
                                                                instanceof
                                                                ZLinkStoreReadFound found)) {
                                                            return completed(ok);
                                                        }
                                                        AuthorityRecord current =
                                                                decode(found.value().bytes());
                                                        AggregateParticipantMarker marker =
                                                                current.aggregate();
                                                        if (marker == null
                                                                || !marker.aggregateId()
                                                                        .equals(fence.aggregateId())
                                                                || marker.aggregateGeneration()
                                                                        != fence
                                                                                .aggregateGeneration()) {
                                                            return completed(ok);
                                                        }
                                                        return clearAggregateMarker(
                                                                        fence,
                                                                        rowKey,
                                                                        found,
                                                                        current,
                                                                        cancellation)
                                                                .thenApply(
                                                                        cleared -> ok && cleared);
                                                    }));
        }
        return chain;
    }

    private CompletionStage<Boolean> clearAggregateMarkersByScan(
            ZLinkAggregateFence fence,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return clearAggregateMarkersByScan(fence, null, cancellation, 0);
    }

    private CompletionStage<Boolean> clearAggregateMarkersByScan(
            ZLinkAggregateFence fence,
            ZLinkStoreScanCursor cursor,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation,
            int restartCount) {
        return provider.scan(
                        new ZLinkStoreScanRequest(AUTHORITY_PREIMAGE_PREFIX, cursor, 1000),
                        cancellation)
                .thenCompose(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                if (restartCount >= 8) {
                                    return failed(
                                            new IllegalStateException(
                                                    "aggregate marker cleanup scan repeatedly"
                                                            + " expired"));
                                }
                                return clearAggregateMarkersByScan(
                                        fence, null, cancellation, restartCount + 1);
                            }
                            ZLinkStoreScanPage page = ((ZLinkStoreScanPageResult) result).value();
                            CompletionStage<Boolean> cleared = completed(true);
                            for (ZLinkStoreScanItem item : page.items()) {
                                AuthorityRecord record = decode(item.value().bytes());
                                AggregateParticipantMarker marker = record.aggregate();
                                if (marker == null
                                        || !marker.aggregateId().equals(fence.aggregateId())
                                        || marker.aggregateGeneration()
                                                != fence.aggregateGeneration()) {
                                    continue;
                                }
                                cleared =
                                        cleared.thenCompose(
                                                ok ->
                                                        clearAggregateMarker(
                                                                        fence,
                                                                        item.key(),
                                                                        new ZLinkStoreReadFound(
                                                                                item.value()),
                                                                        record,
                                                                        cancellation)
                                                                .thenApply(value -> ok && value));
                            }
                            return cleared.thenCompose(
                                    ok ->
                                            page.nextCursor() == null
                                                    ? completed(ok)
                                                    : clearAggregateMarkersByScan(
                                                                    fence,
                                                                    page.nextCursor(),
                                                                    cancellation,
                                                                    restartCount)
                                                            .thenApply(next -> ok && next));
                        });
    }

    private CompletionStage<Boolean> clearAggregateMarker(
            ZLinkAggregateFence fence,
            ZLinkStoreKey rowKey,
            ZLinkStoreReadFound found,
            AuthorityRecord current,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        AggregateParticipantMarker marker = current.aggregate();
        if (marker == null
                || !marker.aggregateId().equals(fence.aggregateId())
                || marker.aggregateGeneration() != fence.aggregateGeneration()) {
            return completed(true);
        }
        return write(
                        new ZLinkStoreWriteRequest(
                                List.of(
                                        new ZLinkStoreVersionCondition(
                                                rowKey, found.value().version())),
                                List.of(
                                        new ZLinkStorePut(
                                                rowKey,
                                                encode(current.withAggregate(null, null)),
                                                null))),
                        cancellation)
                .thenApply(result -> result instanceof ZLinkStoreWriteApplied);
    }

    private CompletionStage<ZLinkAuthorityWriteResult> put(
            ZLinkStoreKey key,
            ZLinkStoreReadFound found,
            AuthorityRecord next,
            List<ZLinkStoreCondition> conditions,
            ZLinkStoreCancellation cancellation,
            String authorityKey,
            ZLinkAuthorityMutation mutation) {
        return spotMembershipFailure(
                        decode(found.value().bytes()).payload(),
                        next.payload(),
                        conditions,
                        adapt(cancellation))
                .thenCompose(
                        failure -> {
                            if (failure != null)
                                return completed(
                                        new ZLinkAuthorityConflict(
                                                snapshot(found.value()), failure));
                            return writeAuthority(
                                    key,
                                    found,
                                    next,
                                    conditions,
                                    List.of(new ZLinkStorePut(key, encode(next), null)),
                                    cancellation,
                                    authorityKey,
                                    mutation);
                        });
    }

    private CompletionStage<ZLinkFrameworkErrorKind> spotMembershipFailure(
            byte[] previousPayload,
            byte[] payload,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        var actor = new ZLinkActorAuthorityPayloadCodec().decode(payload);
        if (actor.isEmpty()
                || actor.get().currentSpotKind()
                        != systems.zlink.framework.spots.ZLinkSpotKind.USER.value()) {
            return completed(null);
        }
        var membership = actor.get();
        var previous = new ZLinkActorAuthorityPayloadCodec().decode(previousPayload);
        if (previous.isPresent()
                && previous.get().state() == ZLinkActorAuthorityPayloadCodec.State.READY
                && previous.get().currentSpotKind() == membership.currentSpotKind()
                && previous.get().currentSpotId().equals(membership.currentSpotId())
                && previous.get().currentSpotGeneration() == membership.currentSpotGeneration())
            return completed(null);
        var key = authorityKey(ZLinkAuthorityKeyCodec.spot(membership.currentSpotId()));
        return provider.read(key, cancellation)
                .thenCompose(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found))
                                return completed(ZLinkFrameworkErrorKind.NOT_FOUND);
                            return projectRead(found, cancellation)
                                    .thenCompose(
                                            projected -> {
                                                var record = (ZLinkAuthoritySnapshot) projected;
                                                var spot =
                                                        new ZLinkServiceAuthorityPayloadCodec()
                                                                .decode(record.payload());
                                                if (!record.storeVersion()
                                                                .equals(
                                                                        found.value()
                                                                                .version()
                                                                                .value())
                                                        || record.objectGeneration()
                                                                != membership
                                                                        .currentSpotGeneration()
                                                        || spot.isEmpty()
                                                        || spot.get().user().isEmpty()
                                                        || !spot.get()
                                                                .spotId()
                                                                .equals(membership.currentSpotId())
                                                        || spot.get().state()
                                                                != ZLinkServiceAuthorityPayloadCodec
                                                                        .State.READY)
                                                    return completed(
                                                            ZLinkFrameworkErrorKind.UNAVAILABLE);
                                                var priorVersion =
                                                        conditions.stream()
                                                                .filter(
                                                                        condition ->
                                                                                condition
                                                                                                instanceof
                                                                                                ZLinkStoreVersionCondition
                                                                                                        version
                                                                                        && version.key()
                                                                                                .equals(
                                                                                                        key))
                                                                .map(
                                                                        condition ->
                                                                                (ZLinkStoreVersionCondition)
                                                                                        condition)
                                                                .findFirst();
                                                if (priorVersion.isPresent()
                                                        && !priorVersion
                                                                .get()
                                                                .expected()
                                                                .equals(found.value().version()))
                                                    return completed(
                                                            ZLinkFrameworkErrorKind.UNAVAILABLE);
                                                if (priorVersion.isEmpty())
                                                    conditions.add(
                                                            new ZLinkStoreVersionCondition(
                                                                    key, found.value().version()));
                                                return provider.read(
                                                                ownerKey(record.ownerId()),
                                                                cancellation)
                                                        .thenApply(
                                                                ownerRead -> {
                                                                    if (!(ownerRead
                                                                                    instanceof
                                                                                    ZLinkStoreReadFound
                                                                                            ownerFound)
                                                                            || ownerGeneration(
                                                                                            ownerFound
                                                                                                    .value()
                                                                                                    .bytes())
                                                                                    != record
                                                                                            .ownerLeaseGeneration()
                                                                            || ownerFound
                                                                                            .value()
                                                                                            .expiresAt()
                                                                                    == null
                                                                            || !ownerFound
                                                                                    .value()
                                                                                    .expiresAt()
                                                                                    .isAfter(
                                                                                            ownerFound
                                                                                                    .value()
                                                                                                    .storeNow()))
                                                                        return ZLinkFrameworkErrorKind
                                                                                .UNAVAILABLE;
                                                                    var required =
                                                                            ZLinkOwnerLeaseRecordCodec
                                                                                    .valueCondition(
                                                                                            record
                                                                                                    .ownerId(),
                                                                                            record
                                                                                                    .ownerLeaseGeneration());
                                                                    var prior =
                                                                            conditions.stream()
                                                                                    .filter(
                                                                                            condition ->
                                                                                                    condition
                                                                                                                    instanceof
                                                                                                                    ZLinkStoreValueCondition
                                                                                                                            value
                                                                                                            && value.key()
                                                                                                                    .equals(
                                                                                                                            required
                                                                                                                                    .key()))
                                                                                    .map(
                                                                                            condition ->
                                                                                                    (ZLinkStoreValueCondition)
                                                                                                            condition)
                                                                                    .findFirst();
                                                                    if (prior.isPresent())
                                                                        return Arrays.equals(
                                                                                        prior.get()
                                                                                                .expected(),
                                                                                        required
                                                                                                .expected())
                                                                                ? null
                                                                                : ZLinkFrameworkErrorKind
                                                                                        .UNAVAILABLE;
                                                                    conditions.add(required);
                                                                    return null;
                                                                });
                                            });
                        });
    }

    private CompletionStage<Boolean> requireAggregateMembership(
            ZLinkAggregatePrepareRequest request,
            List<LoadedParticipant> rows,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        CompletionStage<Boolean> valid = completed(true);
        for (var participant : request.participants()) {
            var actor =
                    new ZLinkActorAuthorityPayloadCodec().decode(participant.authorityPayload());
            if (actor.isPresent()) {
                var target =
                        request.participants().stream()
                                .filter(
                                        spot ->
                                                spot.authorityKey()
                                                        .equals(
                                                                ZLinkAuthorityKeyCodec.spot(
                                                                        actor.get()
                                                                                .currentSpotId())))
                                .findFirst();
                if (target.isPresent()) {
                    if (target.get().objectGeneration() != actor.get().currentSpotGeneration()
                            || new ZLinkServiceAuthorityPayloadCodec()
                                    .decode(target.get().authorityPayload())
                                    .filter(
                                            spot ->
                                                    spot.user().isPresent()
                                                            && spot.spotId()
                                                                    .equals(
                                                                            actor.get()
                                                                                    .currentSpotId()))
                                    .isEmpty()) return completed(false);
                    continue;
                }
            }
            byte[] previousPayload =
                    rows.stream()
                            .filter(
                                    row ->
                                            row.key()
                                                    .equals(
                                                            authorityKey(
                                                                    participant.authorityKey())))
                            .map(row -> decode(row.value().bytes()).payload())
                            .findFirst()
                            .orElseThrow();
            valid =
                    valid.thenCompose(
                            previous ->
                                    previous
                                            ? spotMembershipFailure(
                                                            previousPayload,
                                                            participant.authorityPayload(),
                                                            conditions,
                                                            cancellation)
                                                    .thenApply(failure -> failure == null)
                                            : completed(false));
        }
        return valid;
    }

    private CompletionStage<ZLinkAuthorityWriteResult> writeAuthority(
            ZLinkStoreKey key,
            ZLinkStoreReadFound found,
            AuthorityRecord next,
            List<ZLinkStoreCondition> conditions,
            List<ZLinkStoreMutation> mutations,
            ZLinkStoreCancellation cancellation,
            String authorityKey,
            ZLinkAuthorityMutation mutation) {
        var opaqueCancellation = adapt(cancellation);
        return write(new ZLinkStoreWriteRequest(conditions, mutations), opaqueCancellation)
                .thenCompose(
                        result -> {
                            if (!(result instanceof ZLinkStoreWriteApplied applied)) {
                                AuthorityRecord initial = decode(found.value().bytes());
                                return canRebuild(
                                                key,
                                                found.value().version().value(),
                                                null,
                                                mutation instanceof ZLinkAuthorityPut preserve
                                                                && preserve.preserved() != null
                                                        ? preserve.replacementOwner()
                                                        : new ZLinkLocationOwnerToken(
                                                                initial.ownerId(),
                                                                initial.ownerLeaseGeneration()),
                                                opaqueCancellation)
                                        .thenComposeAsync(
                                                unchanged ->
                                                        unchanged
                                                                ? compareExchange(
                                                                        authorityKey,
                                                                        new ZLinkAuthorityExpectFound(
                                                                                found.value()
                                                                                        .version()
                                                                                        .value()),
                                                                        mutation,
                                                                        cancellation)
                                                                : read(authorityKey, cancellation)
                                                                        .thenApply(
                                                                                ZLinkAuthorityConflict
                                                                                        ::new));
                            }
                            if (mutation instanceof ZLinkAuthorityDelete) {
                                return completed(
                                        new ZLinkAuthorityDeleted(
                                                found.value().version().value(),
                                                applied.storeNow()));
                            }
                            ZLinkStoreVersion version = applied.putVersions().get(key);
                            return completed(stored(next, version.value(), applied.storeNow()));
                        });
    }

    private CompletionStage<Boolean> canRebuild(
            ZLinkStoreKey key,
            String initialVersion,
            String reservationId,
            ZLinkLocationOwnerToken owner,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) return completed(false);
        return provider.read(key, cancellation)
                .thenCompose(
                        read ->
                                canRebuild(read, initialVersion, reservationId)
                                        ? requireLiveOwner(owner, new ArrayList<>(), cancellation)
                                        : completed(false));
    }

    private static boolean canRebuild(
            ZLinkStoreReadResult read, String initialVersion, String reservationId) {
        if (initialVersion == null) return read instanceof ZLinkStoreReadMissing;
        if (!(read instanceof ZLinkStoreReadFound found)
                || !found.value().version().value().equals(initialVersion)) return false;
        return reservationId == null
                || decode(found.value().bytes())
                        .pendingCreation()
                        .map(ZLinkPendingObjectCreation::reservationId)
                        .filter(reservationId::equals)
                        .isPresent();
    }

    private CompletionStage<Boolean> requireLiveOwner(
            AuthorityRecord record,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return requireLiveOwner(
                new ZLinkLocationOwnerToken(record.ownerId(), record.ownerLeaseGeneration()),
                conditions,
                cancellation);
    }

    private CompletionStage<Boolean> requireLiveOwner(
            ZLinkLocationOwnerToken owner,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        ZLinkStoreKey key = ownerKey(owner.ownerId());
        return provider.read(key, cancellation)
                .thenApply(
                        read -> {
                            if (!(read instanceof ZLinkStoreReadFound found)
                                    || ownerGeneration(found.value().bytes())
                                            != owner.leaseGeneration()) {
                                return false;
                            }
                            conditions.add(
                                    ZLinkOwnerLeaseRecordCodec.valueCondition(
                                            owner.ownerId(), owner.leaseGeneration()));
                            return true;
                        });
    }

    private CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return provider.write(request, cancellation)
                .thenApply(
                        result -> {
                            if (result instanceof ZLinkStoreWriteCorrupt) {
                                throw new ZLinkFrameworkException(
                                        ZLinkFrameworkErrorKind.DATA_LOST,
                                        "Location Store capacity record is inconsistent");
                            }
                            return result;
                        });
    }

    private static ZLinkStoreKey generationKey(String counter, String authorityKey) {
        var identity = ZLinkAuthorityKeyCodec.decode(authorityKey);
        return ZLinkOpaqueRecordKey.of("zlink:v12:" + counter, identity.kind(), identity.id());
    }

    private CompletionStage<Counters> nextPair(
            String authorityKey,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        var objectKey = generationKey("object-counter", authorityKey);
        var ownerKey = generationKey("authority-owner-counter", authorityKey);
        return provider.read(objectKey, cancellation)
                .thenCompose(
                        object ->
                                provider.read(ownerKey, cancellation)
                                        .thenCompose(
                                                owner -> {
                                                    CompletionStage<Long> floor =
                                                            object instanceof ZLinkStoreReadFound
                                                                            && owner
                                                                                    instanceof
                                                                                    ZLinkStoreReadFound
                                                                    ? completed(1L)
                                                                    : generationFloor(cancellation);
                                                    return floor.thenApply(
                                                            initial -> {
                                                                var nextObject =
                                                                        issue(
                                                                                objectKey,
                                                                                object,
                                                                                initial,
                                                                                conditions);
                                                                if (nextObject.exhausted()) {
                                                                    return Counters
                                                                            .exhaustedResult();
                                                                }
                                                                var nextOwner =
                                                                        issue(
                                                                                ownerKey,
                                                                                owner,
                                                                                initial,
                                                                                conditions);
                                                                return nextOwner.exhausted()
                                                                        ? Counters.exhaustedResult()
                                                                        : new Counters(
                                                                                nextObject.value(),
                                                                                nextOwner.value(),
                                                                                concat(
                                                                                        nextObject
                                                                                                .mutations(),
                                                                                        nextOwner
                                                                                                .mutations()),
                                                                                false);
                                                            });
                                                }));
    }

    private CompletionStage<Counter> nextCounter(
            ZLinkStoreKey key,
            List<ZLinkStoreCondition> conditions,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return provider.read(key, cancellation)
                .thenCompose(
                        read -> {
                            if (read instanceof ZLinkStoreReadFound) {
                                return completed(issue(key, read, 1L, conditions));
                            }
                            return generationFloor(cancellation)
                                    .thenApply(initial -> issue(key, read, initial, conditions));
                        });
    }

    private CompletionStage<Long> generationFloor(
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        return provider.read(ZLinkLocationStoreFormat.FLOOR, cancellation)
                .thenApply(
                        floor ->
                                floor instanceof ZLinkStoreReadFound found
                                        ? ZLinkLocationStoreFormat.decodeGeneration(
                                                found.value().bytes())
                                        : 1L);
    }

    private static Counter issue(
            ZLinkStoreKey key,
            ZLinkStoreReadResult read,
            long initial,
            List<ZLinkStoreCondition> conditions) {
        long value;
        if (read instanceof ZLinkStoreReadFound found) {
            conditions.add(new ZLinkStoreVersionCondition(key, found.value().version()));
            value = ZLinkLocationStoreFormat.decodeGeneration(found.value().bytes());
        } else {
            conditions.add(new ZLinkStoreMissingCondition(key));
            value = initial;
        }
        return value == Long.MAX_VALUE
                ? Counter.exhaustedResult()
                : new Counter(
                        value,
                        List.of(
                                new ZLinkStorePut(
                                        key,
                                        ZLinkLocationStoreFormat.encodeCounter(value + 1),
                                        null)),
                        false);
    }

    private static ZLinkAuthorityReadResult toRead(ZLinkStoreReadResult result) {
        return result instanceof ZLinkStoreReadMissing missing
                ? new ZLinkAuthorityMissing(missing.storeNow())
                : snapshot(((ZLinkStoreReadFound) result).value());
    }

    private CompletionStage<ZLinkAuthorityReadResult> projectRead(
            ZLinkStoreReadResult result,
            systems.zlink.framework.locationprovider.ZLinkStoreCancellation cancellation) {
        if (!(result instanceof ZLinkStoreReadFound found)) {
            return completed(toRead(result));
        }
        AuthorityRecord record = decode(found.value().bytes());
        AggregateParticipantMarker marker = record.aggregate();
        if (marker == null) {
            return completed(snapshot(found.value()));
        }
        ZLinkAggregateFence fence =
                new ZLinkAggregateFence(marker.aggregateId(), marker.aggregateGeneration());
        return provider.read(aggregateKey(fence), cancellation)
                .thenCompose(
                        aggregateRead -> {
                            if (!(aggregateRead instanceof ZLinkStoreReadFound aggregateFound)) {
                                return failed(
                                        new IllegalStateException(
                                                "aggregate participant references a missing"
                                                        + " aggregate marker"));
                            }
                            PreparedAggregate aggregate =
                                    decodeAggregate(aggregateFound.value().bytes());
                            if (aggregate.state() != AGGREGATE_COMMITTED) {
                                return completed(
                                        snapshot(
                                                record,
                                                marker.expectedStoreVersion(),
                                                found.value().storeNow()));
                            }
                            return aggregateInventory
                                    .readParticipantPayload(fence, marker.index(), cancellation)
                                    .thenApply(
                                            payload -> {
                                                if (!Arrays.equals(
                                                        marker.authorityPayloadSha256(),
                                                        ZLinkAggregateInventoryStore.sha256(
                                                                payload))) {
                                                    throw new IllegalStateException(
                                                            "committed aggregate participant"
                                                                    + " payload checksum is invalid");
                                                }
                                                AuthorityRecord projected =
                                                        marker.ownerTransition()
                                                                        == ZLinkAuthorityGenerationTransition
                                                                                .NEW_OWNER
                                                                ? new AuthorityRecord(
                                                                        payload,
                                                                        record.objectGeneration(),
                                                                        marker
                                                                                .targetAuthorityOwnerGeneration(),
                                                                        aggregate
                                                                                .targetOwner()
                                                                                .ownerId(),
                                                                        aggregate
                                                                                .targetOwner()
                                                                                .leaseGeneration(),
                                                                        new ZLinkPlacementAllocation(
                                                                                ZLinkPlacementAllocationState
                                                                                        .ACTIVE,
                                                                                record.allocation()
                                                                                        .objectKind(),
                                                                                record.allocation()
                                                                                        .stableType(),
                                                                                aggregate
                                                                                        .targetDescriptor(),
                                                                                aggregate
                                                                                        .targetDescriptorLifecycleGeneration(),
                                                                                record.allocation()
                                                                                        .capacityBundle()),
                                                                        Optional.empty())
                                                                : new AuthorityRecord(
                                                                        payload,
                                                                        record.objectGeneration(),
                                                                        record
                                                                                .authorityOwnerGeneration(),
                                                                        record.ownerId(),
                                                                        record
                                                                                .ownerLeaseGeneration(),
                                                                        record.allocation(),
                                                                        record.pendingCreation());
                                                projected =
                                                        projected.withCommittedRelocation(
                                                                record.relocation());
                                                return snapshot(
                                                        projected,
                                                        found.value().version().value(),
                                                        found.value().storeNow());
                                            });
                        });
    }

    private static ZLinkAuthoritySnapshot snapshot(ZLinkStoreValue value) {
        AuthorityRecord record = decode(value.bytes());
        return snapshot(record, value.version().value(), value.storeNow());
    }

    private static ZLinkAuthoritySnapshot snapshot(
            AuthorityRecord record, String version, Instant storeNow) {
        return new ZLinkAuthoritySnapshot(
                version,
                record.payload(),
                record.objectGeneration(),
                record.authorityOwnerGeneration(),
                record.ownerId(),
                record.ownerLeaseGeneration(),
                record.allocation(),
                record.pendingCreation(),
                storeNow,
                record.relocation(),
                record.aggregate() == null ? 0 : record.aggregate().aggregateGeneration());
    }

    private static ZLinkAuthorityStored stored(
            AuthorityRecord record, String version, Instant storeNow) {
        return new ZLinkAuthorityStored(
                version,
                record.payload(),
                record.objectGeneration(),
                record.authorityOwnerGeneration(),
                record.ownerId(),
                record.ownerLeaseGeneration(),
                record.allocation(),
                storeNow);
    }

    private static boolean matches(
            ZLinkStoreValue authority,
            AuthorityRecord current,
            ZLinkObjectReservation reservation) {
        return authority.version().value().equals(reservation.storeVersion())
                && current.objectGeneration() == reservation.objectGeneration()
                && current.authorityOwnerGeneration() == reservation.authorityOwnerGeneration()
                && current.ownerId().equals(reservation.targetOwner().ownerId())
                && current.ownerLeaseGeneration() == reservation.targetOwner().leaseGeneration()
                && current.pendingCreation()
                        .map(ZLinkPendingObjectCreation::reservationId)
                        .filter(reservation.reservationVersion()::equals)
                        .isPresent();
    }

    private static ZLinkPlacementAllocation withState(
            ZLinkPlacementAllocation value, ZLinkPlacementAllocationState state) {
        return new ZLinkPlacementAllocation(
                state,
                value.objectKind(),
                value.stableType(),
                value.descriptor(),
                value.descriptorLifecycleGeneration(),
                value.capacityBundle());
    }

    // --- Authority record canonical JSON (21-location-runtime.md#2.4) ---
    //
    // Top-level: {recordVersion:1, payload(base64), objectGeneration,
    // authorityOwnerGeneration, ownerId, ownerLeaseGeneration, allocation,
    // pendingCreation}. Except for `payload`, integer fields are JSON
    // strings (64-bit values). Aggregate markers and visibleStoreVersion
    // use the shared top-level fields during aggregate recovery.

    static byte[] encode(AuthorityRecord value) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put(FIELD_RECORD_VERSION, AUTHORITY_RECORD_VERSION);
        root.put("payload", Base64.getEncoder().encodeToString(value.payload()));
        root.put("objectGeneration", Long.toUnsignedString(value.objectGeneration()));
        root.put(
                "authorityOwnerGeneration",
                Long.toUnsignedString(value.authorityOwnerGeneration()));
        root.put("ownerId", value.ownerId());
        root.put("ownerLeaseGeneration", Long.toUnsignedString(value.ownerLeaseGeneration()));
        root.set("allocation", encodeAllocation(value.allocation()));
        if (value.pendingCreation().isPresent()) {
            root.set(
                    "pendingCreation",
                    encodePendingCreation(value.pendingCreation().orElseThrow()));
        } else {
            root.putNull("pendingCreation");
        }
        if (value.aggregate() != null || value.relocation() != null) {
            ObjectNode aggregate =
                    value.aggregate() == null
                            ? CANONICAL_JSON.createObjectNode()
                            : encodeAggregateMarker(value.aggregate());
            if (value.relocation() != null) aggregate.set("relocation", value.relocation().json());
            root.set("aggregate", aggregate);
        }
        if (value.visibleStoreVersion() != null) {
            root.put("visibleStoreVersion", value.visibleStoreVersion());
        }
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException("Failed to encode authority record", error);
        }
    }

    private static ObjectNode encodeAllocation(ZLinkPlacementAllocation allocation) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("state", allocationStateWire(allocation.state()));
        node.put("objectKind", authorityObjectKindWire(allocation.objectKind()));
        node.put("stableType", allocation.stableType());
        ObjectNode descriptor = CANONICAL_JSON.createObjectNode();
        descriptor.put("meshName", allocation.descriptor().meshName());
        descriptor.put("routingIdHex", allocation.descriptor().rid().toHex());
        node.set("descriptor", descriptor);
        node.put(
                "descriptorLifecycleGeneration",
                Long.toUnsignedString(allocation.descriptorLifecycleGeneration()));
        node.set("capacity", encodeCapacityBundle(allocation.capacityBundle()));
        return node;
    }

    private static ObjectNode encodeCapacityBundle(ZLinkPlacementCapacityBundle bundle) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("actors", bundle.actorSlots());
        node.put("spots", bundle.spotSlots());
        if (bundle.spotType().isPresent()) {
            ZLinkSpotTypeCapacityDelta delta = bundle.spotType().orElseThrow();
            ObjectNode spotType = CANONICAL_JSON.createObjectNode();
            spotType.put("objectKind", authorityObjectKindWire(delta.objectKind()));
            spotType.put("stableType", delta.stableType());
            spotType.put("count", delta.slots());
            node.set("spotType", spotType);
        } else {
            node.putNull("spotType");
        }
        return node;
    }

    private static ObjectNode encodePendingCreation(ZLinkPendingObjectCreation pending) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("reservationId", pending.reservationId());
        node.put("requestContentReference", pending.requestContentReference());
        node.put("requestSha256", HexFormat.of().formatHex(pending.requestSha256()));
        node.put("requestEncodedSize", pending.requestEncodedSize());
        return node;
    }

    private static ObjectNode encodeAggregateMarker(AggregateParticipantMarker aggregate) {
        ObjectNode marker = CANONICAL_JSON.createObjectNode();
        marker.put("aggregateId", aggregate.aggregateId().toString());
        marker.put("aggregateGeneration", Long.toUnsignedString(aggregate.aggregateGeneration()));
        marker.put("index", aggregate.index());
        marker.put("expectedStoreVersion", aggregate.expectedStoreVersion());
        marker.put(
                "ownerTransition",
                switch (aggregate.ownerTransition()) {
                    case PRESERVE -> OWNER_TRANSITION_PRESERVE;
                    case NEW_OWNER -> OWNER_TRANSITION_NEW_OWNER;
                });
        marker.put(
                "targetAuthorityOwnerGeneration",
                Long.toUnsignedString(aggregate.targetAuthorityOwnerGeneration()));
        marker.put(
                "authorityPayloadSha256",
                HexFormat.of().formatHex(aggregate.authorityPayloadSha256()));
        marker.put(
                "membershipMutationSha256",
                HexFormat.of().formatHex(aggregate.membershipMutationSha256()));
        return marker;
    }

    private static String allocationStateWire(ZLinkPlacementAllocationState state) {
        return switch (state) {
            case PENDING -> "reserved";
            case ACTIVE -> "active";
        };
    }

    private static ZLinkPlacementAllocationState allocationStateFromWire(String value) {
        return switch (value) {
            case "reserved" -> ZLinkPlacementAllocationState.PENDING;
            case "active" -> ZLinkPlacementAllocationState.ACTIVE;
            default -> throw new IllegalStateException("Unrecognized allocation state: " + value);
        };
    }

    private static String authorityObjectKindWire(ZLinkPlacementObjectKind kind) {
        return switch (kind) {
            case ACTOR -> "actor";
            case USER_SPOT -> "userSpot";
            case INSTANCE_SPOT -> "instanceSpot";
        };
    }

    private static ZLinkPlacementObjectKind authorityObjectKindFromWire(String value) {
        return switch (value) {
            case "actor" -> ZLinkPlacementObjectKind.ACTOR;
            case "userSpot" -> ZLinkPlacementObjectKind.USER_SPOT;
            case "instanceSpot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException("Unrecognized objectKind: " + value);
        };
    }

    static AuthorityRecord decode(byte[] bytes) {
        try {
            JsonNode root = CANONICAL_JSON.readTree(bytes);
            if (root.path(FIELD_RECORD_VERSION).asInt(-1) != AUTHORITY_RECORD_VERSION) {
                throw new IllegalStateException(
                        "Location Store authority record has an unrecognized" + " recordVersion");
            }
            byte[] payload = Base64.getDecoder().decode(root.path("payload").asText());
            long objectGeneration = Long.parseUnsignedLong(root.path("objectGeneration").asText());
            long ownerGeneration =
                    Long.parseUnsignedLong(root.path("authorityOwnerGeneration").asText());
            String ownerId = root.path("ownerId").asText();
            long ownerLeaseGeneration =
                    Long.parseUnsignedLong(root.path("ownerLeaseGeneration").asText());
            ZLinkPlacementAllocation allocation = decodeAllocation(root.path("allocation"));
            JsonNode pendingNode = root.path("pendingCreation");
            Optional<ZLinkPendingObjectCreation> pending =
                    pendingNode.isMissingNode() || pendingNode.isNull()
                            ? Optional.empty()
                            : Optional.of(decodePendingCreation(pendingNode));
            JsonNode markerNode = root.path("aggregate");
            if (markerNode.isNull())
                throw new IllegalStateException("Invalid null authority aggregate");
            AggregateParticipantMarker aggregate =
                    markerNode.isMissingNode() ? null : decodeAggregateMarker(markerNode);
            JsonNode visible = root.path("visibleStoreVersion");
            String visibleStoreVersion = visible.isTextual() ? visible.asText() : null;
            return new AuthorityRecord(
                    payload,
                    objectGeneration,
                    ownerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pending,
                    aggregate,
                    visibleStoreVersion,
                    markerNode.has("relocation")
                            ? new ZLinkAuthorityRelocation(markerNode.get("relocation"))
                            : null);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException("Location Store authority record is invalid", failure);
        }
    }

    private static ZLinkPlacementAllocation decodeAllocation(JsonNode node) {
        JsonNode descriptor = node.path("descriptor");
        return new ZLinkPlacementAllocation(
                allocationStateFromWire(node.path("state").asText()),
                authorityObjectKindFromWire(node.path("objectKind").asText()),
                node.path("stableType").asText(),
                new ZLinkMeshNodeDescriptorKey(
                        descriptor.path("meshName").asText(),
                        RoutingId.fromHex(descriptor.path("routingIdHex").asText())),
                Long.parseUnsignedLong(node.path("descriptorLifecycleGeneration").asText()),
                decodeCapacityBundle(node.path("capacity")));
    }

    private static ZLinkPlacementCapacityBundle decodeCapacityBundle(JsonNode node) {
        JsonNode spotTypeNode = node.path("spotType");
        Optional<ZLinkSpotTypeCapacityDelta> spotType =
                spotTypeNode.isMissingNode() || spotTypeNode.isNull()
                        ? Optional.empty()
                        : Optional.of(
                                new ZLinkSpotTypeCapacityDelta(
                                        authorityObjectKindFromWire(
                                                spotTypeNode.path("objectKind").asText()),
                                        spotTypeNode.path("stableType").asText(),
                                        spotTypeNode.path("count").asInt()));
        return new ZLinkPlacementCapacityBundle(
                node.path("actors").asInt(), node.path("spots").asInt(), spotType);
    }

    private static ZLinkPendingObjectCreation decodePendingCreation(JsonNode node) {
        return new ZLinkPendingObjectCreation(
                node.path("reservationId").asText(),
                node.path("requestContentReference").asText(),
                HexFormat.of().parseHex(node.path("requestSha256").asText()),
                node.path("requestEncodedSize").asInt());
    }

    private static AggregateParticipantMarker decodeAggregateMarker(JsonNode node) {
        ZLinkAuthorityRelocation.object(node);
        String[] fields = {
            "aggregateId",
            "aggregateGeneration",
            "index",
            "expectedStoreVersion",
            "ownerTransition",
            "targetAuthorityOwnerGeneration",
            "authorityPayloadSha256",
            "membershipMutationSha256"
        };
        int count = 0;
        for (String field : fields) if (node.has(field)) count++;
        if (count == 0 && node.has("relocation")) return null;
        if (count != fields.length)
            throw new IllegalStateException(
                    "Authority aggregate marker must be complete or absent");
        ZLinkAuthorityRelocation.uuid(node.get("aggregateId"));
        ZLinkAuthorityRelocation.integer(node.get("aggregateGeneration"));
        ZLinkAuthorityRelocation.integer(node.get("targetAuthorityOwnerGeneration"));
        ZLinkAuthorityRelocation.text(node.get("expectedStoreVersion"));
        ZLinkAuthorityRelocation.sha(node.get("authorityPayloadSha256"));
        ZLinkAuthorityRelocation.sha(node.get("membershipMutationSha256"));
        if (!node.path("index").isIntegralNumber()
                || !node.path("index").canConvertToInt()
                || node.path("index").intValue() < 0)
            throw new IllegalStateException("Invalid aggregate index");
        return new AggregateParticipantMarker(
                UUID.fromString(node.path("aggregateId").asText()),
                Long.parseUnsignedLong(node.path("aggregateGeneration").asText()),
                node.path("index").asInt(),
                node.path("expectedStoreVersion").asText(),
                switch (node.path("ownerTransition").asText()) {
                    case OWNER_TRANSITION_PRESERVE -> ZLinkAuthorityGenerationTransition.PRESERVE;
                    case OWNER_TRANSITION_NEW_OWNER -> ZLinkAuthorityGenerationTransition.NEW_OWNER;
                    default -> throw new IllegalStateException("Invalid aggregate ownerTransition");
                },
                Long.parseUnsignedLong(node.path("targetAuthorityOwnerGeneration").asText()),
                HexFormat.of().parseHex(node.path("authorityPayloadSha256").asText()),
                HexFormat.of().parseHex(node.path("membershipMutationSha256").asText()));
    }

    private static byte[] encodeAggregate(byte state, ZLinkAggregatePrepareRequest request) {
        try {
            var bytes = new ByteArrayOutputStream();
            var out = new DataOutputStream(bytes);
            out.writeByte(state);
            out.writeLong(request.aggregateId().getMostSignificantBits());
            out.writeLong(request.aggregateId().getLeastSignificantBits());
            out.writeLong(request.aggregateGeneration());
            out.writeUTF(request.targetDescriptor().meshName());
            out.writeUTF(request.targetDescriptor().rid().toHex());
            out.writeLong(request.targetDescriptorLifecycleGeneration());
            out.writeUTF(request.targetOwner().ownerId());
            out.writeLong(request.targetOwner().leaseGeneration());
            writeBytes(out, request.inventoryDigest());
            out.writeInt(request.capacityBundle().actorSlots());
            out.writeInt(request.capacityBundle().spotSlots());
            out.writeBoolean(request.capacityBundle().spotType().isPresent());
            if (request.capacityBundle().spotType().isPresent()) {
                ZLinkSpotTypeCapacityDelta delta =
                        request.capacityBundle().spotType().orElseThrow();
                out.writeInt(delta.objectKind().ordinal());
                out.writeUTF(delta.stableType());
                out.writeInt(delta.slots());
            }
            out.writeInt(request.participants().size());
            writeBytes(out, ZLinkAggregateInventoryStore.fingerprint(request));
            out.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(failure);
        }
    }

    private static PreparedAggregate decodeAggregate(byte[] bytes) {
        try {
            var in = new DataInputStream(new ByteArrayInputStream(bytes));
            byte state = in.readByte();
            if (state != AGGREGATE_STAGING
                    && state != AGGREGATE_PREPARED
                    && state != AGGREGATE_COMMITTED) {
                throw new IOException("aggregate marker state is invalid");
            }
            var id = new UUID(in.readLong(), in.readLong());
            long generation = in.readLong();
            var descriptor =
                    new ZLinkMeshNodeDescriptorKey(in.readUTF(), RoutingId.fromHex(in.readUTF()));
            long descriptorGeneration = in.readLong();
            var owner = new ZLinkLocationOwnerToken(in.readUTF(), in.readLong());
            byte[] digest = readBytes(in);
            int actors = in.readInt();
            int spots = in.readInt();
            Optional<ZLinkSpotTypeCapacityDelta> spotType =
                    in.readBoolean()
                            ? Optional.of(
                                    new ZLinkSpotTypeCapacityDelta(
                                            ZLinkPlacementObjectKind.values()[in.readInt()],
                                            in.readUTF(),
                                            in.readInt()))
                            : Optional.empty();
            int count = in.readInt();
            if (count < 1) {
                throw new IOException("aggregate participant count is invalid");
            }
            byte[] fingerprint = readBytes(in);
            if (fingerprint.length != 32) {
                throw new IOException("aggregate request fingerprint is invalid");
            }
            if (in.available() != 0) {
                throw new IOException("aggregate marker has trailing bytes");
            }
            return new PreparedAggregate(
                    state,
                    id,
                    generation,
                    descriptor,
                    descriptorGeneration,
                    owner,
                    digest,
                    actors,
                    spots,
                    spotType,
                    count,
                    fingerprint);
        } catch (IOException | RuntimeException failure) {
            throw new IllegalStateException("Location Store aggregate record is invalid", failure);
        }
    }

    private static void writeBytes(DataOutputStream out, byte[] bytes) throws IOException {
        out.writeInt(bytes.length);
        out.write(bytes);
    }

    private static byte[] readBytes(DataInputStream in) throws IOException {
        int length = in.readInt();
        if (length < 0 || length > 64 * 1024 * 1024) {
            throw new IOException("invalid byte length");
        }
        byte[] bytes = in.readNBytes(length);
        if (bytes.length != length) {
            throw new IOException("truncated byte value");
        }
        return bytes;
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "authority\0{actor|spot}\0{Id}". The
    // second segment collapses every Spot kind onto one "spot" string --
    // one Id has exactly one authority row regardless of Spot kind -- and
    // {Id} is the raw ActorId/SpotId, not the "zla1:..." wire form the rest
    // of this class carries as its authority-key contract value.
    private static final String AUTHORITY_PREIMAGE_PREFIX = "authority\0";
    private static final String ACTOR_AUTHORITY_PREIMAGE_PREFIX =
            AUTHORITY_PREIMAGE_PREFIX + "actor\0";

    private static String authorityScanPrefix(String prefix) {
        return switch (prefix) {
            case "" -> AUTHORITY_PREIMAGE_PREFIX;
            case "zla1:a:" -> ACTOR_AUTHORITY_PREIMAGE_PREFIX;
            case "zla1:s:" -> AUTHORITY_PREIMAGE_PREFIX + "spot\0";
            default ->
                    throw new IllegalArgumentException(
                            "unsupported authority scan prefix: " + prefix);
        };
    }

    static ZLinkStoreKey authorityKey(String key) {
        ZLinkAuthorityKeyCodec.AuthorityIdentity identity = ZLinkAuthorityKeyCodec.decode(key);
        return ZLinkOpaqueRecordKey.of("authority", identity.kind(), identity.id());
    }

    // Reverses authorityKey(): recovers the "zla1:..." authority-key
    // contract value from a scanned opaque record's logical key preimage.
    private static String decodeAuthorityKey(ZLinkStoreKey key) {
        String preimage = key.value();
        if (!preimage.startsWith(AUTHORITY_PREIMAGE_PREFIX)) {
            throw new IllegalStateException("authority scan returned a non-authority logical key");
        }
        String rest = preimage.substring(AUTHORITY_PREIMAGE_PREFIX.length());
        int separator = rest.indexOf('\0');
        if (separator < 0) {
            throw new IllegalStateException("authority logical key preimage is malformed");
        }
        String kind = rest.substring(0, separator);
        String id = rest.substring(separator + 1);
        return switch (kind) {
            case "actor" -> ZLinkAuthorityKeyCodec.actor(id);
            case "spot" -> ZLinkAuthorityKeyCodec.spot(id);
            default -> throw new IllegalStateException("unrecognized authority key kind: " + kind);
        };
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "owner-lease\0{OwnerId}".
    private static ZLinkStoreKey ownerKey(String ownerId) {
        return ZLinkOwnerLeaseRecordCodec.key(ownerId);
    }

    private static ZLinkStoreKey aggregateKey(ZLinkAggregateFence fence) {
        return new ZLinkStoreKey(
                AGGREGATE_PREFIX + fence.aggregateId() + ":" + fence.aggregateGeneration());
    }

    private static long ownerGeneration(byte[] bytes) {
        return ZLinkOwnerLeaseRecordCodec.decode(bytes).leaseGeneration();
    }

    // Counter rows are stored as UTF-8 decimal strings, matching the
    // cross-language owner-counter convention (dotnet/cpp/node parse
    // counters with a decimal-string reader; raw big-endian bytes are
    // unreadable to them).
    private static systems.zlink.framework.locationprovider.ZLinkStoreCancellation adapt(
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(cancellation, "cancellation");
        return cancellation::isCancellationRequested;
    }

    private static void requireKey(String key) {
        if (key == null || key.isBlank()) {
            throw new IllegalArgumentException("authority key must not be blank");
        }
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private static boolean sameAggregateRequest(
            PreparedAggregate prepared, ZLinkAggregatePrepareRequest request) {
        return prepared.aggregateId().equals(request.aggregateId())
                && prepared.aggregateGeneration() == request.aggregateGeneration()
                && prepared.targetDescriptor().equals(request.targetDescriptor())
                && prepared.targetDescriptorLifecycleGeneration()
                        == request.targetDescriptorLifecycleGeneration()
                && prepared.targetOwner().equals(request.targetOwner())
                && prepared.capacityBundle().equals(request.capacityBundle())
                && prepared.participantCount() == request.participants().size()
                && Arrays.equals(prepared.inventoryDigest(), request.inventoryDigest())
                && Arrays.equals(
                        prepared.requestFingerprint(),
                        ZLinkAggregateInventoryStore.fingerprint(request));
    }

    private static List<ZLinkStoreMutation> concat(
            List<ZLinkStoreMutation> left, List<ZLinkStoreMutation> right) {
        List<ZLinkStoreMutation> result = new ArrayList<>(left);
        result.addAll(right);
        return result;
    }

    record AuthorityRecord(
            byte[] payload,
            long objectGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkPlacementAllocation allocation,
            Optional<ZLinkPendingObjectCreation> pendingCreation,
            AggregateParticipantMarker aggregate,
            String visibleStoreVersion,
            ZLinkAuthorityRelocation relocation) {
        AuthorityRecord(
                byte[] payload,
                long objectGeneration,
                long authorityOwnerGeneration,
                String ownerId,
                long ownerLeaseGeneration,
                ZLinkPlacementAllocation allocation,
                Optional<ZLinkPendingObjectCreation> pendingCreation,
                AggregateParticipantMarker aggregate,
                String visibleStoreVersion) {
            this(
                    payload,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pendingCreation,
                    aggregate,
                    visibleStoreVersion,
                    null);
        }

        AuthorityRecord(
                byte[] payload,
                long objectGeneration,
                long authorityOwnerGeneration,
                String ownerId,
                long ownerLeaseGeneration,
                ZLinkPlacementAllocation allocation,
                Optional<ZLinkPendingObjectCreation> pendingCreation) {
            this(
                    payload,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pendingCreation,
                    null,
                    null);
        }

        AuthorityRecord {
            payload = payload.clone();
        }

        AuthorityRecord withCommittedRelocation(ZLinkAuthorityRelocation value) {
            return withRelocation(value == null ? null : value.phase("committed"));
        }

        AuthorityRecord withRelocation(ZLinkAuthorityRelocation next) {
            return new AuthorityRecord(
                    payload,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pendingCreation,
                    aggregate,
                    visibleStoreVersion,
                    next);
        }

        AuthorityRecord withPayload(byte[] next) {
            return new AuthorityRecord(
                    next,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pendingCreation,
                    aggregate,
                    visibleStoreVersion,
                    relocation);
        }

        AuthorityRecord withOwner(byte[] next, long generation, ZLinkLocationOwnerToken owner) {
            return new AuthorityRecord(
                    next,
                    objectGeneration,
                    generation,
                    owner.ownerId(),
                    owner.leaseGeneration(),
                    allocation,
                    pendingCreation,
                    aggregate,
                    visibleStoreVersion,
                    relocation);
        }

        AuthorityRecord withAggregate(
                AggregateParticipantMarker next, String nextVisibleStoreVersion) {
            return new AuthorityRecord(
                    payload,
                    objectGeneration,
                    authorityOwnerGeneration,
                    ownerId,
                    ownerLeaseGeneration,
                    allocation,
                    pendingCreation,
                    next,
                    nextVisibleStoreVersion,
                    relocation);
        }
    }

    private record AggregateParticipantMarker(
            UUID aggregateId,
            long aggregateGeneration,
            int index,
            String expectedStoreVersion,
            ZLinkAuthorityGenerationTransition ownerTransition,
            long targetAuthorityOwnerGeneration,
            byte[] authorityPayloadSha256,
            byte[] membershipMutationSha256) {
        AggregateParticipantMarker {
            authorityPayloadSha256 = authorityPayloadSha256.clone();
            membershipMutationSha256 = membershipMutationSha256.clone();
        }

        @Override
        public byte[] authorityPayloadSha256() {
            return authorityPayloadSha256.clone();
        }

        @Override
        public byte[] membershipMutationSha256() {
            return membershipMutationSha256.clone();
        }
    }

    private enum StaleAuthorityReclaim {
        OWNER_LIVE,
        RECLAIMED,
        CONFLICT,
        RECOVERY_REQUIRED,
        NOT_RECLAIMABLE
    }

    private record AggregateCapacityReservation(
            List<ZLinkStoreCondition> conditions, List<ZLinkStoreMutation> mutations) {
        AggregateCapacityReservation {
            conditions = List.copyOf(conditions);
            mutations = List.copyOf(mutations);
        }
    }

    private record Counter(long value, List<ZLinkStoreMutation> mutations, boolean exhausted) {
        private static Counter exhaustedResult() {
            return new Counter(0, List.of(), true);
        }
    }

    private record AggregateMarkerInstallation(
            boolean installed, boolean exhausted, ZLinkAggregatePrepareResult reenteredResult) {
        private static AggregateMarkerInstallation installedResult() {
            return new AggregateMarkerInstallation(true, false, null);
        }

        private static AggregateMarkerInstallation conflictResult() {
            return new AggregateMarkerInstallation(false, false, null);
        }

        private static AggregateMarkerInstallation exhaustedResult() {
            return new AggregateMarkerInstallation(false, true, null);
        }

        private static AggregateMarkerInstallation reenteredResult(
                ZLinkAggregatePrepareResult result) {
            return new AggregateMarkerInstallation(false, false, result);
        }
    }

    private record Counters(
            long objectGeneration,
            long ownerGeneration,
            List<ZLinkStoreMutation> mutations,
            boolean exhausted) {
        private static Counters exhaustedResult() {
            return new Counters(0, 0, List.of(), true);
        }
    }

    private record LoadedParticipant(ZLinkStoreKey key, ZLinkStoreValue value) {}

    private record PreparedAggregate(
            byte state,
            UUID aggregateId,
            long aggregateGeneration,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetDescriptorLifecycleGeneration,
            ZLinkLocationOwnerToken targetOwner,
            byte[] inventoryDigest,
            int actors,
            int spots,
            Optional<ZLinkSpotTypeCapacityDelta> spotType,
            int participantCount,
            byte[] requestFingerprint) {
        PreparedAggregate {
            inventoryDigest = inventoryDigest.clone();
            requestFingerprint = requestFingerprint.clone();
        }

        ZLinkPlacementCapacityBundle capacityBundle() {
            return new ZLinkPlacementCapacityBundle(actors, spots, spotType);
        }

        ZLinkAggregatePrepareRequest request(List<ZLinkAggregateParticipant> participants) {
            if (participants.size() != participantCount) {
                throw new IllegalStateException(
                        "aggregate inventory participant count differs from marker");
            }
            ZLinkAggregatePrepareRequest request =
                    new ZLinkAggregatePrepareRequest(
                            aggregateId,
                            aggregateGeneration,
                            participants,
                            inventoryDigest,
                            targetDescriptor,
                            targetDescriptorLifecycleGeneration,
                            capacityBundle(),
                            targetOwner);
            if (!Arrays.equals(
                    requestFingerprint, ZLinkAggregateInventoryStore.fingerprint(request))) {
                throw new IllegalStateException(
                        "aggregate inventory request fingerprint differs from marker");
            }
            return request;
        }

        @Override
        public byte[] inventoryDigest() {
            return inventoryDigest.clone();
        }

        @Override
        public byte[] requestFingerprint() {
            return requestFingerprint.clone();
        }
    }
}
