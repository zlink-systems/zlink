package systems.zlink.framework.runtime.internal.locations;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BiPredicate;
import java.util.function.Function;

/**
 * Translates Framework descriptor records to opaque provider operations.
 *
 * <p>Descriptor generation and owner fencing remain private Framework data. The provider only sees
 * versioned bytes and atomic conditions.
 */
final class ZLinkProviderDescriptorRepository {
    private static final long STORE_QUERY_TIMEOUT_SECONDS = 5;
    private static final String WIRE_OBJECT_KIND_ACTOR = "actor";
    private static final String WIRE_OBJECT_KIND_USER_SPOT = "userSpot";
    private static final String WIRE_OBJECT_KIND_INSTANCE_SPOT = "instanceSpot";
    private static final String FIELD_ACTIVATION_CONCURRENCY = "activationConcurrency";
    private static final String FIELD_ACTIVE = "active";
    private static final String FIELD_ACTORS = "actors";
    private static final String FIELD_APPLICATION_VERSION = "applicationVersion";
    private static final String FIELD_CAPACITY = "capacity";
    private static final String FIELD_CHANNEL_NAME = "channelName";
    private static final String FIELD_CHANNEL_WEIGHTS = "channelWeights";
    private static final String FIELD_DESCRIPTOR = "descriptor";
    private static final String FIELD_DESCRIPTOR_REVISION = "descriptorRevision";
    private static final String FIELD_ENDPOINT = "endpoint";
    private static final String FIELD_ENTRY_SPOT_ID = "entrySpotId";
    private static final String FIELD_HAS_SNAPSHOT_ADAPTER = "hasSnapshotAdapter";
    private static final String FIELD_LEASE_GENERATION = "leaseGeneration";
    private static final String FIELD_LIFECYCLE_GENERATION = "lifecycleGeneration";
    private static final String FIELD_LIMIT = "limit";
    private static final String FIELD_MAINTENANCE_WAVE = "maintenanceWave";
    private static final String FIELD_MESH_NAME = "meshName";
    private static final String FIELD_OBJECT_CAPABILITIES = "objectCapabilities";
    private static final String FIELD_OBJECT_KIND = "objectKind";
    private static final String FIELD_OBJECT_ROLE = "objectRole";
    private static final String FIELD_OWNER_ID = "ownerId";
    private static final String FIELD_PLACEMENT_WEIGHT = "placementWeight";
    private static final String FIELD_POLICY = "policy";
    private static final String FIELD_PUBLISHER_ROUTING_ID_HEX = "publisherRoutingIdHex";
    private static final int DESCRIPTOR_RECORD_VERSION = 1;
    private static final String FIELD_RECORD_VERSION = "recordVersion";
    private static final String FIELD_RESERVED = "reserved";
    private static final String FIELD_ROUTING_ID_HEX = "routingIdHex";
    private static final String FIELD_SECURITY_IDENTITY = "securityIdentity";
    private static final String FIELD_SERVER_ROUTING_ID_HEX = "serverRoutingIdHex";
    private static final String FIELD_SPOT_TYPES = "spotTypes";
    private static final String FIELD_SPOTS = "spots";
    private static final String FIELD_STABLE_TYPE = "stableType";
    private static final String FIELD_STATE = "state";
    private static final String FIELD_UPDATED_AT_EPOCH_MS = "updatedAtEpochMs";
    private static final String FIELD_WEIGHT = "weight";
    private static final String WIRE_POLICY_DISABLED = "disabled";
    private static final String WIRE_POLICY_RECREATE = "recreate";
    private static final String WIRE_POLICY_SNAPSHOT = "snapshot";
    private static final String WIRE_ROLE_NONE = "none";
    private static final String WIRE_ROLE_CLIENT = "client";
    private static final String WIRE_ROLE_SERVER = "server";
    private static final String WIRE_STATE_PREPARING = "preparing";
    private static final String WIRE_STATE_SERVING = "serving";
    private static final String WIRE_STATE_RELOCATING = "relocating";
    private static final String WIRE_STATE_RELOCATED = "relocated";
    private static final String WIRE_STATE_DRAINING = "draining";
    private static final String WIRE_STATE_STOPPED = "stopped";
    private static final String WIRE_STATE_ERROR = "error";
    private static final String CONTINUATION_VERSION = "v1";
    private static final int CONTINUATION_COMPONENT_COUNT = 3;

    private static final ObjectMapper CANONICAL_JSON = new ObjectMapper();
    private static final int DEFAULT_MESH_PAGE = 100;
    private static final int DEFAULT_CHANNEL_PAGE = 256;
    private static final int MAXIMUM_PAGE_SIZE = 1000;
    private static final int MAXIMUM_CONTINUATION_CHARACTERS = 5600;
    private final ZLinkLocationStore provider;

    ZLinkProviderDescriptorRepository(ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
            ZLinkMeshNodeDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, FIELD_DESCRIPTOR);
        return update(
                meshKey(descriptor.meshName(), descriptor.rid()),
                descriptor.ownerId(),
                descriptor.leaseGeneration(),
                descriptor.lifecycleGeneration(),
                descriptor.descriptorRevision(),
                descriptor,
                intent,
                value -> encodeMeshNodeRecord(value.descriptor()),
                ZLinkProviderDescriptorRepository::decodeMesh,
                ZLinkProviderDescriptorRepository::sameImmutableMesh);
    }

    CompletionStage<ZLinkLocationWriteStatus> removeMeshNode(
            ZLinkMeshNodeDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return remove(
                meshKey(key.meshName(), key.rid()),
                owner,
                bytes -> decodeMesh(bytes).descriptor().ownerId(),
                bytes -> decodeMesh(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
            String meshName, ZLinkPageRequest page) {
        return list(
                meshPrefix(meshName),
                page,
                DEFAULT_MESH_PAGE,
                bytes -> decodeMesh(bytes).descriptor());
    }

    CompletionStage<Optional<ZLinkMeshNodeDescriptor>> readMeshNode(
            ZLinkMeshNodeDescriptorKey key,
            List<ZLinkStoreCondition> conditions,
            ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(key, "key");
        Objects.requireNonNull(cancellation, "cancellation");
        return provider.read(meshKey(key.meshName(), key.rid()), cancellation)
                .thenApply(
                        result -> {
                            if (!(result instanceof ZLinkStoreReadFound found)) {
                                return Optional.empty();
                            }
                            conditions.add(
                                    new ZLinkStoreVersionCondition(
                                            meshKey(key.meshName(), key.rid()),
                                            found.value().version()));
                            return Optional.of(decodeMeshNodeRecord(found.value().bytes()));
                        });
    }

    CompletionStage<ZLinkLocationWriteResult> updateClientServer(
            ZLinkClientServerServerDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, FIELD_DESCRIPTOR);
        return update(
                clientServerKey(descriptor.channelName(), descriptor.serverRid()),
                descriptor.ownerId(),
                descriptor.leaseGeneration(),
                descriptor.lifecycleGeneration(),
                descriptor.descriptorRevision(),
                descriptor,
                intent,
                value -> encodeClientServerRecord(value.descriptor()),
                ZLinkProviderDescriptorRepository::decodeClientServer,
                (current, next) ->
                        current.endpoint().equals(next.endpoint())
                                && current.securityIdentity().equals(next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
            ZLinkClientServerServerDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return remove(
                clientServerKey(key.channelName(), key.serverRid()),
                owner,
                bytes -> decodeClientServer(bytes).descriptor().ownerId(),
                bytes -> decodeClientServer(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkClientServerServerDescriptor>> listClientServers(
            String channelName, ZLinkPageRequest page) {
        return list(
                clientServerPrefix(channelName),
                page,
                DEFAULT_CHANNEL_PAGE,
                bytes -> decodeClientServer(bytes).descriptor());
    }

    CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
            ZLinkFanoutPublisherDescriptor descriptor, ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, FIELD_DESCRIPTOR);
        return update(
                fanoutKey(descriptor.channelName(), descriptor.publisherRid()),
                descriptor.ownerId(),
                descriptor.leaseGeneration(),
                descriptor.lifecycleGeneration(),
                descriptor.descriptorRevision(),
                descriptor,
                intent,
                value -> encodeFanoutPublisherRecord(value.descriptor()),
                ZLinkProviderDescriptorRepository::decodeFanout,
                (current, next) ->
                        current.endpoint().equals(next.endpoint())
                                && current.securityIdentity().equals(next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
            ZLinkFanoutPublisherDescriptorKey key, ZLinkLocationOwnerToken owner) {
        return remove(
                fanoutKey(key.channelName(), key.publisherRid()),
                owner,
                bytes -> decodeFanout(bytes).descriptor().ownerId(),
                bytes -> decodeFanout(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>> listFanoutPublishers(
            String channelName, ZLinkPageRequest page) {
        return list(
                fanoutPrefix(channelName),
                page,
                DEFAULT_CHANNEL_PAGE,
                bytes -> decodeFanout(bytes).descriptor());
    }

    private <T> CompletionStage<ZLinkLocationWriteResult> update(
            ZLinkStoreKey rowKey,
            String ownerId,
            long leaseGeneration,
            long lifecycleGeneration,
            long descriptorRevision,
            T descriptor,
            ZLinkLocationWriteIntent intent,
            Function<StoredDescriptor<T>, byte[]> encode,
            Function<byte[], StoredDescriptor<T>> decode,
            BiPredicate<T, T> immutableFieldsEqual) {
        Objects.requireNonNull(intent, "intent");
        ZLinkStoreKey leaseKey = ownerKey(ownerId);
        return provider.read(leaseKey, active())
                .thenCompose(
                        lease -> {
                            if (!(lease instanceof ZLinkStoreReadFound liveLease)
                                    || decodeOwnerGeneration(liveLease.value().bytes())
                                            != leaseGeneration) {
                                return completed(ZLinkLocationWriteResult.ignoredStale());
                            }
                            return provider.read(rowKey, active())
                                    .thenCompose(
                                            current -> {
                                                long generation = 1;
                                                ZLinkStoreCondition rowCondition;
                                                if (current instanceof ZLinkStoreReadFound found) {
                                                    StoredDescriptor<T> record =
                                                            decode.apply(found.value().bytes());
                                                    generation = record.generation();
                                                    T stored = record.descriptor();
                                                    DescriptorIdentity identity = identity(stored);
                                                    boolean renew =
                                                            intent == ZLinkLocationWriteIntent.RENEW
                                                                    && identity.ownerId()
                                                                            .equals(ownerId)
                                                                    && identity.leaseGeneration()
                                                                            == leaseGeneration
                                                                    && identity
                                                                                    .lifecycleGeneration()
                                                                            == lifecycleGeneration
                                                                    && Long.compareUnsigned(
                                                                                    descriptorRevision,
                                                                                    identity
                                                                                            .descriptorRevision())
                                                                            > 0
                                                                    && immutableFieldsEqual.test(
                                                                            stored, descriptor);
                                                    if (!renew) {
                                                        return provider.read(
                                                                        ownerKey(
                                                                                identity.ownerId()),
                                                                        active())
                                                                .thenCompose(
                                                                        previousOwner -> {
                                                                            if (previousOwner
                                                                                    instanceof
                                                                                    ZLinkStoreReadFound) {
                                                                                return completed(
                                                                                        intent
                                                                                                        == ZLinkLocationWriteIntent
                                                                                                                .NEW_CLAIM
                                                                                                ? ZLinkLocationWriteResult
                                                                                                        .rejectedConflict()
                                                                                                : ZLinkLocationWriteResult
                                                                                                        .ignoredStale());
                                                                            }
                                                                            return writeDescriptor(
                                                                                    rowKey,
                                                                                    ownerId,
                                                                                    leaseGeneration,
                                                                                    new ZLinkStoreVersionCondition(
                                                                                            rowKey,
                                                                                            found.value()
                                                                                                    .version()),
                                                                                    encode.apply(
                                                                                            new StoredDescriptor<>(
                                                                                                    Math
                                                                                                            .addExact(
                                                                                                                    record
                                                                                                                            .generation(),
                                                                                                                    1),
                                                                                                    descriptor)),
                                                                                    Math.addExact(
                                                                                            record
                                                                                                    .generation(),
                                                                                            1));
                                                                        });
                                                    }
                                                    rowCondition =
                                                            new ZLinkStoreVersionCondition(
                                                                    rowKey,
                                                                    found.value().version());
                                                } else {
                                                    if (intent == ZLinkLocationWriteIntent.RENEW) {
                                                        return completed(
                                                                ZLinkLocationWriteResult
                                                                        .ignoredStale());
                                                    }
                                                    rowCondition =
                                                            new ZLinkStoreMissingCondition(rowKey);
                                                }
                                                byte[] encoded =
                                                        encode.apply(
                                                                new StoredDescriptor<>(
                                                                        generation, descriptor));
                                                return writeDescriptor(
                                                        rowKey,
                                                        ownerId,
                                                        leaseGeneration,
                                                        rowCondition,
                                                        encoded,
                                                        generation);
                                            });
                        });
    }

    private CompletionStage<ZLinkLocationWriteResult> writeDescriptor(
            ZLinkStoreKey rowKey,
            String ownerId,
            long leaseGeneration,
            ZLinkStoreCondition rowCondition,
            byte[] encoded,
            long generation) {
        var request =
                new ZLinkStoreWriteRequest(
                        List.of(
                                ZLinkOwnerLeaseRecordCodec.valueCondition(ownerId, leaseGeneration),
                                rowCondition),
                        List.of(new ZLinkStorePut(rowKey, encoded, null)));
        CompletionStage<systems.zlink.framework.locationprovider.ZLinkStoreWriteResult> write;
        try {
            write = provider.write(request, active());
        } catch (Throwable failure) {
            return reconcile(rowKey, encoded, generation, failure);
        }
        return write.handle(
                        (result, failure) -> {
                            if (failure != null) {
                                return reconcile(rowKey, encoded, generation, unwrap(failure));
                            }
                            if (result instanceof ZLinkStoreWriteApplied applied) {
                                return completed(
                                        ZLinkLocationWriteResult.stored(
                                                generation, applied.storeNow()));
                            }
                            return completed(ZLinkLocationWriteResult.ignoredStale());
                        })
                .thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkLocationWriteResult> reconcile(
            ZLinkStoreKey rowKey, byte[] expected, long generation, Throwable originalFailure) {
        return provider.read(rowKey, active())
                .toCompletableFuture()
                .orTimeout(STORE_QUERY_TIMEOUT_SECONDS, TimeUnit.SECONDS)
                .handle(
                        (result, failure) -> {
                            if (failure == null
                                    && result instanceof ZLinkStoreReadFound found
                                    && Arrays.equals(expected, found.value().bytes())) {
                                return ZLinkLocationWriteResult.stored(
                                        generation, found.value().storeNow());
                            }
                            throw new CompletionException(originalFailure);
                        });
    }

    private CompletionStage<ZLinkLocationWriteStatus> remove(
            ZLinkStoreKey rowKey,
            ZLinkLocationOwnerToken owner,
            Function<byte[], String> ownerId,
            Function<byte[], Long> leaseGeneration) {
        Objects.requireNonNull(owner, "owner");
        return provider.read(rowKey, active())
                .thenCompose(
                        current -> {
                            if (!(current instanceof ZLinkStoreReadFound found)
                                    || !owner.ownerId().equals(ownerId.apply(found.value().bytes()))
                                    || owner.leaseGeneration()
                                            != leaseGeneration.apply(found.value().bytes())) {
                                return completed(ZLinkLocationWriteStatus.IGNORED_STALE);
                            }
                            return provider.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreVersionCondition(
                                                                    rowKey,
                                                                    found.value().version())),
                                                    List.of(new ZLinkStoreDelete(rowKey))),
                                            active())
                                    .thenApply(
                                            result ->
                                                    result instanceof ZLinkStoreWriteApplied
                                                            ? ZLinkLocationWriteStatus.STORED
                                                            : ZLinkLocationWriteStatus
                                                                    .IGNORED_STALE);
                        });
    }

    private <T> CompletionStage<ZLinkLocationPage<T>> list(
            String prefix,
            ZLinkPageRequest request,
            int defaultPageSize,
            Function<byte[], T> decode) {
        Objects.requireNonNull(request, "request");
        int pageSize = request.pageSize() <= 0 ? defaultPageSize : request.pageSize();
        if (pageSize < 1 || pageSize > MAXIMUM_PAGE_SIZE) {
            throw new IllegalArgumentException(
                    "pageSize must be in the range 1.." + MAXIMUM_PAGE_SIZE);
        }
        ZLinkStoreScanCursor cursor =
                request.continuationToken() == null
                        ? null
                        : decodeContinuation(prefix, request.continuationToken());
        return provider.scan(new ZLinkStoreScanRequest(prefix, cursor, pageSize), active())
                .thenApply(
                        result -> {
                            if (result instanceof ZLinkStoreScanExpired) {
                                throw new CompletionException(
                                        new IOException("Location Store snapshot has expired."));
                            }
                            var page = ((ZLinkStoreScanPageResult) result).value();
                            if (page.items().size() > pageSize) {
                                throw new IllegalStateException(
                                        "Location Store returned more rows than requested.");
                            }
                            List<T> items =
                                    page.items().stream()
                                            .map(item -> decode.apply(item.value().bytes()))
                                            .toList();
                            return new ZLinkLocationPage<>(
                                    items,
                                    page.nextCursor() == null
                                            ? null
                                            : encodeContinuation(prefix, page.nextCursor()));
                        });
    }

    // --- MeshNode descriptor canonical JSON (21-location-runtime.md#2.4) ---
    //
    // Top-level: {recordVersion:1, ownerId, leaseGeneration, descriptorRevision, descriptor}.
    // `descriptor`'s field set is pinned by the glossary-derived table in
    // 21-location-runtime.md#2.4 and the store-record-v1 golden fixture's
    // "meshNodeDescriptor-normal" vector. 64-bit generation/revision fields
    // are JSON strings (JSON numbers can't losslessly carry them); bounded
    // 32-bit counts (weights, limits, capacity usage) are JSON numbers.

    private static byte[] encodeMeshNodeRecord(ZLinkMeshNodeDescriptor descriptor) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put(FIELD_RECORD_VERSION, DESCRIPTOR_RECORD_VERSION);
        root.put(FIELD_OWNER_ID, descriptor.ownerId());
        root.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        root.put(FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        root.set(FIELD_DESCRIPTOR, encodeMeshNodePayload(descriptor));
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException("Failed to encode MeshNode descriptor record", error);
        }
    }

    private static ObjectNode encodeMeshNodePayload(ZLinkMeshNodeDescriptor descriptor) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put(FIELD_MESH_NAME, descriptor.meshName());
        node.put(FIELD_ROUTING_ID_HEX, descriptor.rid().toHex());
        node.put(
                FIELD_LIFECYCLE_GENERATION,
                Long.toUnsignedString(descriptor.lifecycleGeneration()));
        node.put(FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        node.put(FIELD_ENDPOINT, descriptor.endpoint());
        if (descriptor.entrySpotId().isPresent()) {
            node.put(FIELD_ENTRY_SPOT_ID, descriptor.entrySpotId().get());
        } else {
            node.putNull(FIELD_ENTRY_SPOT_ID);
        }
        ObjectNode channelWeights = CANONICAL_JSON.createObjectNode();
        descriptor.channelWeights().entrySet().stream()
                .sorted(Map.Entry.comparingByKey())
                .forEach(entry -> channelWeights.put(entry.getKey(), entry.getValue()));
        node.set(FIELD_CHANNEL_WEIGHTS, channelWeights);
        node.put(FIELD_APPLICATION_VERSION, Long.toUnsignedString(descriptor.applicationVersion()));
        ArrayNode capabilities = CANONICAL_JSON.createArrayNode();
        descriptor
                .objectCapabilities()
                .forEach(
                        capability -> {
                            ObjectNode encoded = CANONICAL_JSON.createObjectNode();
                            encoded.put(FIELD_OBJECT_KIND, objectKindWire(capability.objectKind()));
                            encoded.put(FIELD_STABLE_TYPE, capability.stableType());
                            encoded.put(FIELD_POLICY, policyWire(capability.policy()));
                            encoded.put(
                                    FIELD_HAS_SNAPSHOT_ADAPTER, capability.hasSnapshotAdapter());
                            encoded.put(FIELD_LIMIT, capability.spotLimit());
                            capabilities.add(encoded);
                        });
        node.set(FIELD_OBJECT_CAPABILITIES, capabilities);
        node.put(FIELD_OBJECT_ROLE, objectRoleWire(descriptor.objectRole()));
        node.put(FIELD_PLACEMENT_WEIGHT, descriptor.placementWeight());
        node.set(FIELD_CAPACITY, encodeCapacity(descriptor.capacity()));
        ObjectNode activation = CANONICAL_JSON.createObjectNode();
        activation.put(FIELD_ACTIVE, descriptor.activationConcurrency().active());
        activation.put(FIELD_LIMIT, descriptor.activationConcurrency().limit());
        node.set(FIELD_ACTIVATION_CONCURRENCY, activation);
        if (descriptor.maintenanceWave().isPresent()) {
            node.put(FIELD_MAINTENANCE_WAVE, descriptor.maintenanceWave().get());
        } else {
            node.putNull(FIELD_MAINTENANCE_WAVE);
        }
        node.put(FIELD_STATE, stateWire(descriptor.state()));
        node.put(FIELD_SECURITY_IDENTITY, descriptor.securityIdentity());
        node.put(FIELD_OWNER_ID, descriptor.ownerId());
        node.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        node.put(FIELD_UPDATED_AT_EPOCH_MS, Long.toString(descriptor.updatedAt().toEpochMilli()));
        return node;
    }

    private static ObjectNode encodeCapacity(ZLinkPlacementCapacity capacity) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.set(FIELD_ACTORS, encodeUsage(capacity.actors()));
        node.set(FIELD_SPOTS, encodeUsage(capacity.spots()));
        ArrayNode spotTypes = CANONICAL_JSON.createArrayNode();
        capacity.spotTypes()
                .forEach(
                        spotType -> {
                            ObjectNode encoded = CANONICAL_JSON.createObjectNode();
                            encoded.put(FIELD_OBJECT_KIND, objectKindWire(spotType.objectKind()));
                            encoded.put(FIELD_STABLE_TYPE, spotType.stableType());
                            encoded.put(FIELD_ACTIVE, spotType.usage().active());
                            encoded.put(FIELD_RESERVED, spotType.usage().reserved());
                            encoded.put(FIELD_LIMIT, spotType.usage().limit());
                            spotTypes.add(encoded);
                        });
        node.set(FIELD_SPOT_TYPES, spotTypes);
        return node;
    }

    private static ObjectNode encodeUsage(ZLinkCapacityUsage usage) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put(FIELD_ACTIVE, usage.active());
        node.put(FIELD_RESERVED, usage.reserved());
        node.put(FIELD_LIMIT, usage.limit());
        return node;
    }

    static ZLinkMeshNodeDescriptor decodeMeshNodeRecord(byte[] bytes) {
        JsonNode root;
        try {
            root = CANONICAL_JSON.readTree(bytes);
        } catch (IOException error) {
            throw new IllegalStateException("Location descriptor record is invalid", error);
        }
        if (root.path(FIELD_RECORD_VERSION).asInt(-1) != DESCRIPTOR_RECORD_VERSION) {
            throw new IllegalStateException(
                    "Location descriptor record has an unrecognized" + " recordVersion");
        }
        JsonNode descriptor = root.path(FIELD_DESCRIPTOR);
        Map<String, Integer> channelWeights = new LinkedHashMap<>();
        descriptor
                .path(FIELD_CHANNEL_WEIGHTS)
                .fields()
                .forEachRemaining(
                        entry -> channelWeights.put(entry.getKey(), entry.getValue().asInt()));
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        descriptor
                .path(FIELD_OBJECT_CAPABILITIES)
                .forEach(
                        capability ->
                                capabilities.add(
                                        new ZLinkObjectCapability(
                                                objectKindFromWire(
                                                        capability
                                                                .path(FIELD_OBJECT_KIND)
                                                                .asText()),
                                                capability.path(FIELD_STABLE_TYPE).asText(),
                                                policyFromWire(
                                                        capability.path(FIELD_POLICY).asText()),
                                                capability
                                                        .path(FIELD_HAS_SNAPSHOT_ADAPTER)
                                                        .asBoolean(),
                                                capability.path(FIELD_LIMIT).asInt())));
        JsonNode capacityNode = descriptor.path(FIELD_CAPACITY);
        List<ZLinkSpotTypeCapacity> spotTypes = new ArrayList<>();
        capacityNode
                .path(FIELD_SPOT_TYPES)
                .forEach(
                        spotType ->
                                spotTypes.add(
                                        new ZLinkSpotTypeCapacity(
                                                objectKindFromWire(
                                                        spotType.path(FIELD_OBJECT_KIND).asText()),
                                                spotType.path(FIELD_STABLE_TYPE).asText(),
                                                decodeUsage(spotType))));
        ZLinkPlacementCapacity capacity =
                new ZLinkPlacementCapacity(
                        decodeUsage(capacityNode.path(FIELD_ACTORS)),
                        decodeUsage(capacityNode.path(FIELD_SPOTS)),
                        spotTypes);
        JsonNode activation = descriptor.path(FIELD_ACTIVATION_CONCURRENCY);
        JsonNode entrySpotId = descriptor.path(FIELD_ENTRY_SPOT_ID);
        JsonNode maintenanceWave = descriptor.path(FIELD_MAINTENANCE_WAVE);
        return new ZLinkMeshNodeDescriptor(
                descriptor.path(FIELD_MESH_NAME).asText(),
                RoutingId.fromHex(descriptor.path(FIELD_ROUTING_ID_HEX).asText()),
                Long.parseUnsignedLong(descriptor.path(FIELD_LIFECYCLE_GENERATION).asText()),
                Long.parseUnsignedLong(descriptor.path(FIELD_DESCRIPTOR_REVISION).asText()),
                descriptor.path(FIELD_ENDPOINT).asText(),
                channelWeights,
                Long.parseUnsignedLong(descriptor.path(FIELD_APPLICATION_VERSION).asText()),
                capabilities,
                objectRoleFromWire(descriptor.path(FIELD_OBJECT_ROLE).asText()),
                entrySpotId.isMissingNode() || entrySpotId.isNull()
                        ? Optional.empty()
                        : Optional.of(entrySpotId.asText()),
                descriptor.path(FIELD_PLACEMENT_WEIGHT).asInt(),
                capacity,
                new ZLinkActivationConcurrency(
                        activation.path(FIELD_ACTIVE).asInt(),
                        activation.path(FIELD_LIMIT).asInt()),
                maintenanceWave.isMissingNode() || maintenanceWave.isNull()
                        ? Optional.empty()
                        : Optional.of(maintenanceWave.asText()),
                stateFromWire(descriptor.path(FIELD_STATE).asText()),
                descriptor.path(FIELD_SECURITY_IDENTITY).asText(),
                descriptor.path(FIELD_OWNER_ID).asText(),
                Long.parseUnsignedLong(descriptor.path(FIELD_LEASE_GENERATION).asText()),
                Instant.ofEpochMilli(
                        Long.parseLong(descriptor.path(FIELD_UPDATED_AT_EPOCH_MS).asText())));
    }

    private static ZLinkCapacityUsage decodeUsage(JsonNode node) {
        return new ZLinkCapacityUsage(
                node.path(FIELD_ACTIVE).asInt(),
                node.path(FIELD_RESERVED).asInt(),
                node.path(FIELD_LIMIT).asInt());
    }

    private static String objectKindWire(ZLinkPlacementObjectKind kind) {
        return switch (kind) {
            case ACTOR -> WIRE_OBJECT_KIND_ACTOR;
            case USER_SPOT -> WIRE_OBJECT_KIND_USER_SPOT;
            case INSTANCE_SPOT -> WIRE_OBJECT_KIND_INSTANCE_SPOT;
        };
    }

    private static ZLinkPlacementObjectKind objectKindFromWire(String value) {
        return switch (value) {
            case WIRE_OBJECT_KIND_ACTOR -> ZLinkPlacementObjectKind.ACTOR;
            case WIRE_OBJECT_KIND_USER_SPOT -> ZLinkPlacementObjectKind.USER_SPOT;
            case WIRE_OBJECT_KIND_INSTANCE_SPOT -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException("Unrecognized objectKind: " + value);
        };
    }

    private static String policyWire(ZLinkObjectMaintenancePolicyKind policy) {
        return switch (policy) {
            case DISABLED -> WIRE_POLICY_DISABLED;
            case RECREATE -> WIRE_POLICY_RECREATE;
            case SNAPSHOT -> WIRE_POLICY_SNAPSHOT;
        };
    }

    private static ZLinkObjectMaintenancePolicyKind policyFromWire(String value) {
        return switch (value) {
            case WIRE_POLICY_DISABLED -> ZLinkObjectMaintenancePolicyKind.DISABLED;
            case WIRE_POLICY_RECREATE -> ZLinkObjectMaintenancePolicyKind.RECREATE;
            case WIRE_POLICY_SNAPSHOT -> ZLinkObjectMaintenancePolicyKind.SNAPSHOT;
            default -> throw new IllegalStateException("Unrecognized policy: " + value);
        };
    }

    private static String objectRoleWire(ZLinkMeshNodeObjectRole role) {
        return switch (role) {
            case NONE -> WIRE_ROLE_NONE;
            case CLIENT -> WIRE_ROLE_CLIENT;
            case SERVER -> WIRE_ROLE_SERVER;
        };
    }

    private static ZLinkMeshNodeObjectRole objectRoleFromWire(String value) {
        return switch (value) {
            case WIRE_ROLE_NONE -> ZLinkMeshNodeObjectRole.NONE;
            case WIRE_ROLE_CLIENT -> ZLinkMeshNodeObjectRole.CLIENT;
            case WIRE_ROLE_SERVER -> ZLinkMeshNodeObjectRole.SERVER;
            default -> throw new IllegalStateException("Unrecognized objectRole: " + value);
        };
    }

    private static String stateWire(ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> WIRE_STATE_PREPARING;
            case SERVING -> WIRE_STATE_SERVING;
            case RELOCATING -> WIRE_STATE_RELOCATING;
            case RELOCATED -> WIRE_STATE_RELOCATED;
            case DRAINING -> WIRE_STATE_DRAINING;
            case STOPPED -> WIRE_STATE_STOPPED;
            case ERROR -> WIRE_STATE_ERROR;
        };
    }

    private static ZLinkFrameworkRuntimeState stateFromWire(String value) {
        return switch (value) {
            case WIRE_STATE_PREPARING -> ZLinkFrameworkRuntimeState.PREPARING;
            case WIRE_STATE_SERVING -> ZLinkFrameworkRuntimeState.SERVING;
            case WIRE_STATE_RELOCATING -> ZLinkFrameworkRuntimeState.RELOCATING;
            case WIRE_STATE_RELOCATED -> ZLinkFrameworkRuntimeState.RELOCATED;
            case WIRE_STATE_DRAINING -> ZLinkFrameworkRuntimeState.DRAINING;
            case WIRE_STATE_STOPPED -> ZLinkFrameworkRuntimeState.STOPPED;
            case WIRE_STATE_ERROR -> ZLinkFrameworkRuntimeState.ERROR;
            default -> throw new IllegalStateException("Unrecognized state: " + value);
        };
    }

    private static StoredDescriptor<ZLinkMeshNodeDescriptor> decodeMesh(byte[] bytes) {
        // No provider-internal generation is carried in the canonical
        // payload (21-location-runtime.md#2.4): the opaque record's own
        // store version already serves as the CAS fence, so this value is
        // an unused placeholder kept only for the shared update()/list()
        // scaffolding's signature.
        return new StoredDescriptor<>(0, decodeMeshNodeRecord(bytes));
    }

    // --- ClientServer server descriptor canonical JSON
    // (21-location-runtime.md#2.4) ---

    private static byte[] encodeClientServerRecord(ZLinkClientServerServerDescriptor descriptor) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put(FIELD_RECORD_VERSION, DESCRIPTOR_RECORD_VERSION);
        root.put(FIELD_OWNER_ID, descriptor.ownerId());
        root.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        root.put(FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        ObjectNode payload = CANONICAL_JSON.createObjectNode();
        payload.put(FIELD_CHANNEL_NAME, descriptor.channelName());
        payload.put(FIELD_SERVER_ROUTING_ID_HEX, descriptor.serverRid().toHex());
        payload.put(
                FIELD_LIFECYCLE_GENERATION,
                Long.toUnsignedString(descriptor.lifecycleGeneration()));
        payload.put(
                FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        payload.put(FIELD_ENDPOINT, descriptor.endpoint());
        payload.put(FIELD_WEIGHT, descriptor.weight());
        payload.put(FIELD_STATE, stateWire(descriptor.state()));
        payload.put(FIELD_SECURITY_IDENTITY, descriptor.securityIdentity());
        payload.put(FIELD_OWNER_ID, descriptor.ownerId());
        payload.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        payload.put(
                FIELD_UPDATED_AT_EPOCH_MS, Long.toString(descriptor.updatedAt().toEpochMilli()));
        root.set(FIELD_DESCRIPTOR, payload);
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                    "Failed to encode ClientServer server descriptor record", error);
        }
    }

    private static StoredDescriptor<ZLinkClientServerServerDescriptor> decodeClientServer(
            byte[] bytes) {
        JsonNode root;
        try {
            root = CANONICAL_JSON.readTree(bytes);
        } catch (IOException error) {
            throw new IllegalStateException("Location descriptor record is invalid", error);
        }
        if (root.path(FIELD_RECORD_VERSION).asInt(-1) != DESCRIPTOR_RECORD_VERSION) {
            throw new IllegalStateException(
                    "Location descriptor record has an unrecognized" + " recordVersion");
        }
        JsonNode descriptor = root.path(FIELD_DESCRIPTOR);
        return new StoredDescriptor<>(
                0,
                new ZLinkClientServerServerDescriptor(
                        descriptor.path(FIELD_CHANNEL_NAME).asText(),
                        RoutingId.fromHex(descriptor.path(FIELD_SERVER_ROUTING_ID_HEX).asText()),
                        Long.parseUnsignedLong(
                                descriptor.path(FIELD_LIFECYCLE_GENERATION).asText()),
                        Long.parseUnsignedLong(descriptor.path(FIELD_DESCRIPTOR_REVISION).asText()),
                        descriptor.path(FIELD_ENDPOINT).asText(),
                        descriptor.path(FIELD_WEIGHT).asInt(),
                        stateFromWire(descriptor.path(FIELD_STATE).asText()),
                        descriptor.path(FIELD_SECURITY_IDENTITY).asText(),
                        descriptor.path(FIELD_OWNER_ID).asText(),
                        Long.parseUnsignedLong(descriptor.path(FIELD_LEASE_GENERATION).asText()),
                        Instant.ofEpochMilli(
                                Long.parseLong(
                                        descriptor.path(FIELD_UPDATED_AT_EPOCH_MS).asText()))));
    }

    // --- Fanout publisher descriptor canonical JSON
    // (21-location-runtime.md#2.4) --- same fields as the ClientServer
    // server descriptor minus `weight`.

    private static byte[] encodeFanoutPublisherRecord(ZLinkFanoutPublisherDescriptor descriptor) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put(FIELD_RECORD_VERSION, DESCRIPTOR_RECORD_VERSION);
        root.put(FIELD_OWNER_ID, descriptor.ownerId());
        root.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        root.put(FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        ObjectNode payload = CANONICAL_JSON.createObjectNode();
        payload.put(FIELD_CHANNEL_NAME, descriptor.channelName());
        payload.put(FIELD_PUBLISHER_ROUTING_ID_HEX, descriptor.publisherRid().toHex());
        payload.put(
                FIELD_LIFECYCLE_GENERATION,
                Long.toUnsignedString(descriptor.lifecycleGeneration()));
        payload.put(
                FIELD_DESCRIPTOR_REVISION, Long.toUnsignedString(descriptor.descriptorRevision()));
        payload.put(FIELD_ENDPOINT, descriptor.endpoint());
        payload.put(FIELD_STATE, stateWire(descriptor.state()));
        payload.put(FIELD_SECURITY_IDENTITY, descriptor.securityIdentity());
        payload.put(FIELD_OWNER_ID, descriptor.ownerId());
        payload.put(FIELD_LEASE_GENERATION, Long.toUnsignedString(descriptor.leaseGeneration()));
        payload.put(
                FIELD_UPDATED_AT_EPOCH_MS, Long.toString(descriptor.updatedAt().toEpochMilli()));
        root.set(FIELD_DESCRIPTOR, payload);
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                    "Failed to encode fanout publisher descriptor record", error);
        }
    }

    private static StoredDescriptor<ZLinkFanoutPublisherDescriptor> decodeFanout(byte[] bytes) {
        JsonNode root;
        try {
            root = CANONICAL_JSON.readTree(bytes);
        } catch (IOException error) {
            throw new IllegalStateException("Location descriptor record is invalid", error);
        }
        if (root.path(FIELD_RECORD_VERSION).asInt(-1) != DESCRIPTOR_RECORD_VERSION) {
            throw new IllegalStateException(
                    "Location descriptor record has an unrecognized" + " recordVersion");
        }
        JsonNode descriptor = root.path(FIELD_DESCRIPTOR);
        return new StoredDescriptor<>(
                0,
                new ZLinkFanoutPublisherDescriptor(
                        descriptor.path(FIELD_CHANNEL_NAME).asText(),
                        RoutingId.fromHex(descriptor.path(FIELD_PUBLISHER_ROUTING_ID_HEX).asText()),
                        Long.parseUnsignedLong(
                                descriptor.path(FIELD_LIFECYCLE_GENERATION).asText()),
                        Long.parseUnsignedLong(descriptor.path(FIELD_DESCRIPTOR_REVISION).asText()),
                        descriptor.path(FIELD_ENDPOINT).asText(),
                        stateFromWire(descriptor.path(FIELD_STATE).asText()),
                        descriptor.path(FIELD_SECURITY_IDENTITY).asText(),
                        descriptor.path(FIELD_OWNER_ID).asText(),
                        Long.parseUnsignedLong(descriptor.path(FIELD_LEASE_GENERATION).asText()),
                        Instant.ofEpochMilli(
                                Long.parseLong(
                                        descriptor.path(FIELD_UPDATED_AT_EPOCH_MS).asText()))));
    }

    private static DescriptorIdentity identity(Object descriptor) {
        if (descriptor instanceof ZLinkMeshNodeDescriptor value) {
            return new DescriptorIdentity(
                    value.ownerId(),
                    value.leaseGeneration(),
                    value.lifecycleGeneration(),
                    value.descriptorRevision());
        }
        if (descriptor instanceof ZLinkClientServerServerDescriptor value) {
            return new DescriptorIdentity(
                    value.ownerId(),
                    value.leaseGeneration(),
                    value.lifecycleGeneration(),
                    value.descriptorRevision());
        }
        ZLinkFanoutPublisherDescriptor value = (ZLinkFanoutPublisherDescriptor) descriptor;
        return new DescriptorIdentity(
                value.ownerId(),
                value.leaseGeneration(),
                value.lifecycleGeneration(),
                value.descriptorRevision());
    }

    private static boolean sameImmutableMesh(
            ZLinkMeshNodeDescriptor current, ZLinkMeshNodeDescriptor next) {
        return current.meshName().equals(next.meshName())
                && current.rid().equals(next.rid())
                && current.lifecycleGeneration() == next.lifecycleGeneration()
                && current.endpoint().equals(next.endpoint())
                && current.channelWeights().keySet().equals(next.channelWeights().keySet())
                && current.applicationVersion() == next.applicationVersion()
                && current.objectCapabilities().equals(next.objectCapabilities())
                && current.objectRole() == next.objectRole()
                && current.entrySpotId().equals(next.entrySpotId())
                && current.capacity().actors().limit() == next.capacity().actors().limit()
                && current.capacity().spots().limit() == next.capacity().spots().limit()
                && current.securityIdentity().equals(next.securityIdentity())
                && current.ownerId().equals(next.ownerId())
                && current.leaseGeneration() == next.leaseGeneration();
    }

    private static long decodeOwnerGeneration(byte[] bytes) {
        return ZLinkOwnerLeaseRecordCodec.decode(bytes).leaseGeneration();
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "owner-lease\0{OwnerId}".
    private static ZLinkStoreKey ownerKey(String ownerId) {
        return ZLinkOwnerLeaseRecordCodec.key(ownerId);
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4): "mesh-node\0{MeshName}\0{hex(RoutingId)}",
    // NUL-separated, no length prefix. A runtime in any language derives
    // the same SHA-256-hashed opaque record key from this same string.
    static ZLinkStoreKey meshKey(String meshName, RoutingId rid) {
        return ZLinkOpaqueRecordKey.of("mesh-node", meshName, rid.toHex());
    }

    private static String meshPrefix(String meshName) {
        return ZLinkOpaqueRecordKey.of("mesh-node", meshName).value() + "\0";
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4):
    // "client-server\0{ChannelName}\0{hex(RoutingId)}".
    private static ZLinkStoreKey clientServerKey(String channelName, RoutingId rid) {
        return ZLinkOpaqueRecordKey.of("client-server", channelName, rid.toHex());
    }

    private static String clientServerPrefix(String channelName) {
        return ZLinkOpaqueRecordKey.of("client-server", channelName).value() + "\0";
    }

    // Canonical cross-language logical key preimage
    // (21-location-runtime.md#2.4):
    // "fanout-publisher\0{ChannelName}\0{hex(RoutingId)}".
    private static ZLinkStoreKey fanoutKey(String channelName, RoutingId rid) {
        return ZLinkOpaqueRecordKey.of("fanout-publisher", channelName, rid.toHex());
    }

    private static String fanoutPrefix(String channelName) {
        return ZLinkOpaqueRecordKey.of("fanout-publisher", channelName).value() + "\0";
    }

    private static String encodeContinuation(String prefix, ZLinkStoreScanCursor cursor) {
        byte[] cursorBytes = cursor.value().getBytes(StandardCharsets.UTF_8);
        if (cursorBytes.length < 1 || cursorBytes.length > 4096) {
            throw new IllegalStateException("Location Store returned an invalid snapshot cursor.");
        }
        return CONTINUATION_VERSION + "." + base64(digest(prefix)) + "." + base64(cursorBytes);
    }

    private static ZLinkStoreScanCursor decodeContinuation(String prefix, String token) {
        if (token.length() < 1 || token.length() > MAXIMUM_CONTINUATION_CHARACTERS) {
            throw invalidContinuation();
        }
        String[] parts = token.split("\\.", CONTINUATION_COMPONENT_COUNT);
        if (parts.length != CONTINUATION_COMPONENT_COUNT
                || !CONTINUATION_VERSION.equals(parts[0])) {
            throw invalidContinuation();
        }
        try {
            byte[] expected = digest(prefix);
            byte[] actual = decodeBase64(parts[1]);
            byte[] cursor = decodeBase64(parts[2]);
            if (!MessageDigest.isEqual(expected, actual)
                    || cursor.length < 1
                    || cursor.length > 4096) {
                throw invalidContinuation();
            }
            String value =
                    StandardCharsets.UTF_8
                            .newDecoder()
                            .onMalformedInput(CodingErrorAction.REPORT)
                            .onUnmappableCharacter(CodingErrorAction.REPORT)
                            .decode(ByteBuffer.wrap(cursor))
                            .toString();
            return new ZLinkStoreScanCursor(value);
        } catch (IllegalArgumentException | CharacterCodingException error) {
            throw invalidContinuation();
        }
    }

    private static byte[] digest(String prefix) {
        try {
            return MessageDigest.getInstance("SHA-256")
                    .digest(prefix.getBytes(StandardCharsets.UTF_8));
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException("SHA-256 is required", error);
        }
    }

    private static String base64(byte[] bytes) {
        return Base64.getUrlEncoder().withoutPadding().encodeToString(bytes);
    }

    private static byte[] decodeBase64(String value) {
        return Base64.getUrlDecoder().decode(value);
    }

    private static IllegalArgumentException invalidContinuation() {
        return new IllegalArgumentException("Location Store continuation token is invalid.");
    }

    private static systems.zlink.framework.locationprovider.ZLinkStoreCancellation active() {
        return () -> false;
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion && completion.getCause() != null
                ? completion.getCause()
                : failure;
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record StoredDescriptor<T>(long generation, T descriptor) {}

    private record DescriptorIdentity(
            String ownerId,
            long leaseGeneration,
            long lifecycleGeneration,
            long descriptorRevision) {}
}
