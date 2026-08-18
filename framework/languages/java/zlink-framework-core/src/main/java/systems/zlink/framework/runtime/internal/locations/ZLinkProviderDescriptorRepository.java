package systems.zlink.framework.runtime.internal.locations;
import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.util.Arrays;
import java.util.Optional;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.BiPredicate;
import java.util.function.Function;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.locationprovider.ZLinkStoreCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreScanCursor;
import systems.zlink.framework.locationprovider.ZLinkStoreScanExpired;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkLocationPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.locations.ZLinkPageRequest;

/**
 * Translates Framework descriptor records to opaque provider operations.
 *
 * <p>Descriptor generation and owner fencing remain private Framework data.
 * The provider only sees versioned bytes and atomic conditions.</p>
 */
final class ZLinkProviderDescriptorRepository {
    private static final String PREFIX = "zlink:v11:";
    private static final ObjectMapper CANONICAL_JSON = new ObjectMapper();
    private static final int OWNER_LEASE_WRITE_RETRIES = 3;
    private static final int DEFAULT_MESH_PAGE = 100;
    private static final int DEFAULT_CHANNEL_PAGE = 256;
    private static final int MAXIMUM_PAGE_SIZE = 1000;
    private static final int MAXIMUM_CONTINUATION_CHARACTERS = 5600;
    private final ZLinkLocationStore provider;

    ZLinkProviderDescriptorRepository(ZLinkLocationStore provider) {
        this.provider = Objects.requireNonNull(provider, "provider");
    }

    CompletionStage<ZLinkLocationWriteResult> updateMeshNode(
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
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
        ZLinkMeshNodeDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            meshKey(key.meshName(), key.rid()),
            owner,
            bytes -> decodeMesh(bytes).descriptor().ownerId(),
            bytes -> decodeMesh(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkMeshNodeDescriptor>> listMeshNodes(
        String meshName,
        ZLinkPageRequest page) {
        return list(
            meshPrefix(meshName),
            page,
            DEFAULT_MESH_PAGE,
            bytes -> decodeMesh(bytes).descriptor());
    }

    CompletionStage<Optional<ZLinkMeshNodeDescriptor>> readMeshNode(
        ZLinkMeshNodeDescriptorKey key,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(key, "key");
        Objects.requireNonNull(cancellation, "cancellation");
        return provider.read(meshKey(key.meshName(), key.rid()), cancellation)
            .thenApply(result -> result instanceof ZLinkStoreReadFound found
                ? Optional.of(decodeMeshNodeRecord(found.value().bytes()))
                : Optional.empty());
    }

    CompletionStage<ZLinkLocationWriteResult> updateClientServer(
        ZLinkClientServerServerDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        return update(
            clientServerKey(
                descriptor.channelName(),
                descriptor.serverRid()),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor,
            intent,
            value -> encode(
                value.generation(),
                ZLinkLocationDescriptorCodec.serializeClientServer(
                    value.descriptor())),
            bytes -> {
                Envelope envelope = decodeEnvelope(bytes);
                return new StoredDescriptor<>(
                    envelope.generation(),
                    ZLinkLocationDescriptorCodec.deserializeClientServer(
                        envelope.json(),
                        0,
                        extractUpdatedAt(envelope.json())));
            },
            (current, next) ->
                current.endpoint().equals(next.endpoint())
                    && current.securityIdentity().equals(
                        next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeClientServer(
        ZLinkClientServerServerDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            clientServerKey(key.channelName(), key.serverRid()),
            owner,
            bytes -> decodeClientServer(bytes).descriptor().ownerId(),
            bytes -> decodeClientServer(bytes)
                .descriptor().leaseGeneration());
    }

    CompletionStage<
        ZLinkLocationPage<ZLinkClientServerServerDescriptor>>
        listClientServers(String channelName, ZLinkPageRequest page) {
        return list(
            clientServerPrefix(channelName),
            page,
            DEFAULT_CHANNEL_PAGE,
            bytes -> decodeClientServer(bytes).descriptor());
    }

    CompletionStage<ZLinkLocationWriteResult> updateFanoutPublisher(
        ZLinkFanoutPublisherDescriptor descriptor,
        ZLinkLocationWriteIntent intent) {
        Objects.requireNonNull(descriptor, "descriptor");
        return update(
            fanoutKey(descriptor.channelName(), descriptor.publisherRid()),
            descriptor.ownerId(),
            descriptor.leaseGeneration(),
            descriptor.lifecycleGeneration(),
            descriptor.descriptorRevision(),
            descriptor,
            intent,
            value -> encode(
                value.generation(),
                ZLinkLocationDescriptorCodec.serializeFanoutPublisher(
                    value.descriptor())),
            bytes -> {
                Envelope envelope = decodeEnvelope(bytes);
                return new StoredDescriptor<>(
                    envelope.generation(),
                    ZLinkLocationDescriptorCodec.deserializeFanoutPublisher(
                        envelope.json(),
                        0,
                        extractUpdatedAt(envelope.json())));
            },
            (current, next) ->
                current.endpoint().equals(next.endpoint())
                    && current.securityIdentity().equals(
                        next.securityIdentity()));
    }

    CompletionStage<ZLinkLocationWriteStatus> removeFanoutPublisher(
        ZLinkFanoutPublisherDescriptorKey key,
        ZLinkLocationOwnerToken owner) {
        return remove(
            fanoutKey(key.channelName(), key.publisherRid()),
            owner,
            bytes -> decodeFanout(bytes).descriptor().ownerId(),
            bytes -> decodeFanout(bytes).descriptor().leaseGeneration());
    }

    CompletionStage<ZLinkLocationPage<ZLinkFanoutPublisherDescriptor>>
        listFanoutPublishers(String channelName, ZLinkPageRequest page) {
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
        return provider.read(leaseKey, active()).thenCompose(lease -> {
            if (!(lease instanceof ZLinkStoreReadFound liveLease)
                || decodeOwnerGeneration(liveLease.value().bytes())
                    != leaseGeneration) {
                return completed(
                    ZLinkLocationWriteResult.ignoredStale());
            }
            return provider.read(rowKey, active()).thenCompose(current -> {
                long generation = 1;
                ZLinkStoreCondition rowCondition;
                if (current instanceof ZLinkStoreReadFound found) {
                    StoredDescriptor<T> record =
                        decode.apply(found.value().bytes());
                    generation = record.generation();
                    T stored = record.descriptor();
                    DescriptorIdentity identity =
                        identity(stored);
                    boolean renew =
                        intent == ZLinkLocationWriteIntent.RENEW
                            && identity.ownerId().equals(ownerId)
                            && identity.leaseGeneration()
                                == leaseGeneration
                            && identity.lifecycleGeneration()
                                == lifecycleGeneration
                            && descriptorRevision
                                > identity.descriptorRevision()
                            && immutableFieldsEqual.test(
                                stored,
                                descriptor);
                    if (!renew) {
                        if (intent
                            == ZLinkLocationWriteIntent.NEW_CLAIM) {
                            return completed(
                                ZLinkLocationWriteResult
                                    .rejectedConflict());
                        }
                        return provider.read(
                                ownerKey(identity.ownerId()),
                                active())
                            .thenCompose(previousOwner -> {
                                if (previousOwner
                                    instanceof ZLinkStoreReadFound) {
                                    return completed(
                                        ZLinkLocationWriteResult
                                            .ignoredStale());
                                }
                                return writeDescriptor(
                                    rowKey,
                                    liveLease,
                                    new ZLinkStoreVersionCondition(
                                        rowKey,
                                        found.value().version()),
                                    encode.apply(
                                        new StoredDescriptor<>(
                                            Math.addExact(
                                                record.generation(),
                                                1),
                                            descriptor)),
                                    Math.addExact(
                                        record.generation(),
                                        1),
                                    OWNER_LEASE_WRITE_RETRIES);
                            });
                    }
                    rowCondition = new ZLinkStoreVersionCondition(
                        rowKey,
                        found.value().version());
                } else {
                    if (intent == ZLinkLocationWriteIntent.RENEW) {
                        return completed(
                            ZLinkLocationWriteResult.ignoredStale());
                    }
                    rowCondition =
                        new ZLinkStoreMissingCondition(rowKey);
                }
                byte[] encoded = encode.apply(
                    new StoredDescriptor<>(generation, descriptor));
                return writeDescriptor(
                    rowKey,
                    liveLease,
                    rowCondition,
                    encoded,
                    generation,
                    OWNER_LEASE_WRITE_RETRIES);
            });
        });
    }

    private CompletionStage<ZLinkLocationWriteResult> writeDescriptor(
        ZLinkStoreKey rowKey,
        ZLinkStoreReadFound liveLease,
        ZLinkStoreCondition rowCondition,
        byte[] encoded,
        long generation,
        int retriesRemaining) {
        var request = new ZLinkStoreWriteRequest(
            List.of(
                new ZLinkStoreVersionCondition(
                    ownerKey(decodeOwnerId(
                        liveLease.value().bytes())),
                    liveLease.value().version()),
                rowCondition),
            List.of(new ZLinkStorePut(rowKey, encoded, null)));
        CompletionStage<systems.zlink.framework.locationprovider
            .ZLinkStoreWriteResult> write;
        try {
            write = provider.write(request, active());
        } catch (Throwable failure) {
            return reconcile(rowKey, encoded, generation, failure);
        }
        return write.handle((result, failure) -> {
            if (failure != null) {
                return reconcile(
                    rowKey,
                    encoded,
                    generation,
                    unwrap(failure));
            }
            if (result instanceof ZLinkStoreWriteApplied applied) {
                return completed(ZLinkLocationWriteResult.stored(
                    generation,
                    applied.storeNow()));
            }
            if (retriesRemaining <= 0) {
                return completed(ZLinkLocationWriteResult.ignoredStale());
            }
            String ownerId = decodeOwnerId(liveLease.value().bytes());
            long leaseGeneration =
                decodeOwnerGeneration(liveLease.value().bytes());
            return provider.read(ownerKey(ownerId), active())
                .thenCompose(currentLease -> {
                    if (!(currentLease
                            instanceof ZLinkStoreReadFound refreshed)
                        || decodeOwnerGeneration(
                                refreshed.value().bytes())
                            != leaseGeneration) {
                        return completed(
                            ZLinkLocationWriteResult.ignoredStale());
                    }
                    return writeDescriptor(
                        rowKey,
                        refreshed,
                        rowCondition,
                        encoded,
                        generation,
                        retriesRemaining - 1);
                });
        }).thenCompose(stage -> stage);
    }

    private CompletionStage<ZLinkLocationWriteResult> reconcile(
        ZLinkStoreKey rowKey,
        byte[] expected,
        long generation,
        Throwable originalFailure) {
        return provider.read(rowKey, active())
            .toCompletableFuture()
            .orTimeout(5, TimeUnit.SECONDS)
            .handle((result, failure) -> {
                if (failure == null
                    && result instanceof ZLinkStoreReadFound found
                    && Arrays.equals(
                        expected,
                        found.value().bytes())) {
                    return ZLinkLocationWriteResult.stored(
                        generation,
                        found.value().storeNow());
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
        return provider.read(rowKey, active()).thenCompose(current -> {
            if (!(current instanceof ZLinkStoreReadFound found)
                || !owner.ownerId().equals(
                    ownerId.apply(found.value().bytes()))
                || owner.leaseGeneration()
                    != leaseGeneration.apply(found.value().bytes())) {
                return completed(
                    ZLinkLocationWriteStatus.IGNORED_STALE);
            }
            return provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreVersionCondition(
                            rowKey,
                            found.value().version())),
                        List.of(new ZLinkStoreDelete(rowKey))),
                    active())
                .thenApply(result -> result
                    instanceof ZLinkStoreWriteApplied
                    ? ZLinkLocationWriteStatus.STORED
                    : ZLinkLocationWriteStatus.IGNORED_STALE);
        });
    }

    private <T> CompletionStage<ZLinkLocationPage<T>> list(
        String prefix,
        ZLinkPageRequest request,
        int defaultPageSize,
        Function<byte[], T> decode) {
        Objects.requireNonNull(request, "request");
        int pageSize = request.pageSize() <= 0
            ? defaultPageSize
            : request.pageSize();
        if (pageSize < 1 || pageSize > MAXIMUM_PAGE_SIZE) {
            throw new IllegalArgumentException(
                "pageSize must be in the range 1..1000");
        }
        ZLinkStoreScanCursor cursor =
            request.continuationToken() == null
                ? null
                : decodeContinuation(
                    prefix,
                    request.continuationToken());
        return provider.scan(
                new ZLinkStoreScanRequest(
                    prefix,
                    cursor,
                    pageSize),
                active())
            .thenApply(result -> {
                if (result instanceof ZLinkStoreScanExpired) {
                    throw new CompletionException(new IOException(
                        "Location Store snapshot has expired."));
                }
                var page = ((ZLinkStoreScanPageResult) result).value();
                if (page.items().size() > pageSize) {
                    throw new IllegalStateException(
                        "Location Store returned more rows than requested.");
                }
                List<T> items = page.items().stream()
                    .map(item -> decode.apply(
                        item.value().bytes()))
                    .toList();
                return new ZLinkLocationPage<>(
                    items,
                    page.nextCursor() == null
                        ? null
                        : encodeContinuation(
                            prefix,
                            page.nextCursor()));
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

    private static byte[] encodeMeshNodeRecord(
        ZLinkMeshNodeDescriptor descriptor) {
        ObjectNode root = CANONICAL_JSON.createObjectNode();
        root.put("recordVersion", 1);
        root.put("ownerId", descriptor.ownerId());
        root.put(
            "leaseGeneration",
            Long.toString(descriptor.leaseGeneration()));
        root.put(
            "descriptorRevision",
            Long.toString(descriptor.descriptorRevision()));
        root.set("descriptor", encodeMeshNodePayload(descriptor));
        try {
            return CANONICAL_JSON.writeValueAsBytes(root);
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                "Failed to encode MeshNode descriptor record", error);
        }
    }

    private static ObjectNode encodeMeshNodePayload(
        ZLinkMeshNodeDescriptor descriptor) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("meshName", descriptor.meshName());
        node.put("routingIdHex", descriptor.rid().toHex());
        node.put(
            "lifecycleGeneration",
            Long.toString(descriptor.lifecycleGeneration()));
        node.put(
            "descriptorRevision",
            Long.toString(descriptor.descriptorRevision()));
        node.put("endpoint", descriptor.endpoint());
        if (descriptor.entrySpotId().isPresent()) {
            node.put("entrySpotId", descriptor.entrySpotId().get());
        } else {
            node.putNull("entrySpotId");
        }
        ObjectNode channelWeights = CANONICAL_JSON.createObjectNode();
        descriptor.channelWeights().entrySet().stream()
            .sorted(Map.Entry.comparingByKey())
            .forEach(entry -> channelWeights.put(
                entry.getKey(), entry.getValue()));
        node.set("channelWeights", channelWeights);
        node.put(
            "applicationVersion",
            Long.toString(descriptor.applicationVersion()));
        ArrayNode capabilities = CANONICAL_JSON.createArrayNode();
        descriptor.objectCapabilities().forEach(capability -> {
            ObjectNode encoded = CANONICAL_JSON.createObjectNode();
            encoded.put(
                "objectKind", objectKindWire(capability.objectKind()));
            encoded.put("stableType", capability.stableType());
            encoded.put("policy", policyWire(capability.policy()));
            encoded.put(
                "hasSnapshotAdapter", capability.hasSnapshotAdapter());
            encoded.put("limit", capability.spotLimit());
            capabilities.add(encoded);
        });
        node.set("objectCapabilities", capabilities);
        node.put("objectRole", objectRoleWire(descriptor.objectRole()));
        node.put("placementWeight", descriptor.placementWeight());
        node.set("capacity", encodeCapacity(descriptor.capacity()));
        ObjectNode activation = CANONICAL_JSON.createObjectNode();
        activation.put(
            "active", descriptor.activationConcurrency().active());
        activation.put(
            "limit", descriptor.activationConcurrency().limit());
        node.set("activationConcurrency", activation);
        if (descriptor.maintenanceWave().isPresent()) {
            node.put("maintenanceWave", descriptor.maintenanceWave().get());
        } else {
            node.putNull("maintenanceWave");
        }
        node.put("state", stateWire(descriptor.state()));
        node.put("securityIdentity", descriptor.securityIdentity());
        node.put("ownerId", descriptor.ownerId());
        node.put(
            "leaseGeneration",
            Long.toString(descriptor.leaseGeneration()));
        node.put(
            "updatedAtEpochMs",
            Long.toString(descriptor.updatedAt().toEpochMilli()));
        return node;
    }

    private static ObjectNode encodeCapacity(ZLinkPlacementCapacity capacity) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.set("actors", encodeUsage(capacity.actors()));
        node.set("spots", encodeUsage(capacity.spots()));
        ArrayNode spotTypes = CANONICAL_JSON.createArrayNode();
        capacity.spotTypes().forEach(spotType -> {
            ObjectNode encoded = CANONICAL_JSON.createObjectNode();
            encoded.put(
                "objectKind", objectKindWire(spotType.objectKind()));
            encoded.put("stableType", spotType.stableType());
            encoded.put("active", spotType.usage().active());
            encoded.put("reserved", spotType.usage().reserved());
            encoded.put("limit", spotType.usage().limit());
            spotTypes.add(encoded);
        });
        node.set("spotTypes", spotTypes);
        return node;
    }

    private static ObjectNode encodeUsage(ZLinkCapacityUsage usage) {
        ObjectNode node = CANONICAL_JSON.createObjectNode();
        node.put("active", usage.active());
        node.put("reserved", usage.reserved());
        node.put("limit", usage.limit());
        return node;
    }

    private static ZLinkMeshNodeDescriptor decodeMeshNodeRecord(
        byte[] bytes) {
        JsonNode root;
        try {
            root = CANONICAL_JSON.readTree(bytes);
        } catch (IOException error) {
            throw new IllegalStateException(
                "Location descriptor record is invalid", error);
        }
        if (root.path("recordVersion").asInt(-1) != 1) {
            throw new IllegalStateException(
                "Location descriptor record has an unrecognized"
                    + " recordVersion");
        }
        JsonNode descriptor = root.path("descriptor");
        Map<String, Integer> channelWeights = new LinkedHashMap<>();
        descriptor.path("channelWeights").fields().forEachRemaining(
            entry -> channelWeights.put(
                entry.getKey(), entry.getValue().asInt()));
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        descriptor.path("objectCapabilities").forEach(capability ->
            capabilities.add(new ZLinkObjectCapability(
                objectKindFromWire(
                    capability.path("objectKind").asText()),
                capability.path("stableType").asText(),
                policyFromWire(capability.path("policy").asText()),
                capability.path("hasSnapshotAdapter").asBoolean(),
                capability.path("limit").asInt())));
        JsonNode capacityNode = descriptor.path("capacity");
        List<ZLinkSpotTypeCapacity> spotTypes = new ArrayList<>();
        capacityNode.path("spotTypes").forEach(spotType ->
            spotTypes.add(new ZLinkSpotTypeCapacity(
                objectKindFromWire(
                    spotType.path("objectKind").asText()),
                spotType.path("stableType").asText(),
                decodeUsage(spotType))));
        ZLinkPlacementCapacity capacity = new ZLinkPlacementCapacity(
            decodeUsage(capacityNode.path("actors")),
            decodeUsage(capacityNode.path("spots")),
            spotTypes);
        JsonNode activation = descriptor.path("activationConcurrency");
        JsonNode entrySpotId = descriptor.path("entrySpotId");
        JsonNode maintenanceWave = descriptor.path("maintenanceWave");
        return new ZLinkMeshNodeDescriptor(
            descriptor.path("meshName").asText(),
            RoutingId.fromHex(descriptor.path("routingIdHex").asText()),
            Long.parseLong(
                descriptor.path("lifecycleGeneration").asText()),
            Long.parseLong(
                descriptor.path("descriptorRevision").asText()),
            descriptor.path("endpoint").asText(),
            channelWeights,
            Long.parseLong(
                descriptor.path("applicationVersion").asText()),
            capabilities,
            objectRoleFromWire(descriptor.path("objectRole").asText()),
            entrySpotId.isMissingNode() || entrySpotId.isNull()
                ? Optional.empty()
                : Optional.of(entrySpotId.asText()),
            descriptor.path("placementWeight").asInt(),
            capacity,
            new ZLinkActivationConcurrency(
                activation.path("active").asInt(),
                activation.path("limit").asInt()),
            maintenanceWave.isMissingNode() || maintenanceWave.isNull()
                ? Optional.empty()
                : Optional.of(maintenanceWave.asText()),
            stateFromWire(descriptor.path("state").asText()),
            descriptor.path("securityIdentity").asText(),
            descriptor.path("ownerId").asText(),
            Long.parseLong(descriptor.path("leaseGeneration").asText()),
            Instant.ofEpochMilli(
                Long.parseLong(
                    descriptor.path("updatedAtEpochMs").asText())));
    }

    private static ZLinkCapacityUsage decodeUsage(JsonNode node) {
        return new ZLinkCapacityUsage(
            node.path("active").asInt(),
            node.path("reserved").asInt(),
            node.path("limit").asInt());
    }

    private static String objectKindWire(ZLinkPlacementObjectKind kind) {
        return switch (kind) {
            case ACTOR -> "actor";
            case USER_SPOT -> "userSpot";
            case INSTANCE_SPOT -> "instanceSpot";
        };
    }

    private static ZLinkPlacementObjectKind objectKindFromWire(
        String value) {
        return switch (value) {
            case "actor" -> ZLinkPlacementObjectKind.ACTOR;
            case "userSpot" -> ZLinkPlacementObjectKind.USER_SPOT;
            case "instanceSpot" -> ZLinkPlacementObjectKind.INSTANCE_SPOT;
            default -> throw new IllegalStateException(
                "Unrecognized objectKind: " + value);
        };
    }

    private static String policyWire(ZLinkObjectMaintenancePolicyKind policy) {
        return switch (policy) {
            case DISABLED -> "disabled";
            case RECREATE -> "recreate";
            case SNAPSHOT -> "snapshot";
        };
    }

    private static ZLinkObjectMaintenancePolicyKind policyFromWire(
        String value) {
        return switch (value) {
            case "disabled" -> ZLinkObjectMaintenancePolicyKind.DISABLED;
            case "recreate" -> ZLinkObjectMaintenancePolicyKind.RECREATE;
            case "snapshot" -> ZLinkObjectMaintenancePolicyKind.SNAPSHOT;
            default -> throw new IllegalStateException(
                "Unrecognized policy: " + value);
        };
    }

    private static String objectRoleWire(ZLinkMeshNodeObjectRole role) {
        return switch (role) {
            case NONE -> "none";
            case CLIENT -> "client";
            case SERVER -> "server";
        };
    }

    private static ZLinkMeshNodeObjectRole objectRoleFromWire(
        String value) {
        return switch (value) {
            case "none" -> ZLinkMeshNodeObjectRole.NONE;
            case "client" -> ZLinkMeshNodeObjectRole.CLIENT;
            case "server" -> ZLinkMeshNodeObjectRole.SERVER;
            default -> throw new IllegalStateException(
                "Unrecognized objectRole: " + value);
        };
    }

    private static String stateWire(ZLinkFrameworkRuntimeState state) {
        return switch (state) {
            case PREPARING -> "preparing";
            case SERVING -> "serving";
            case RELOCATING -> "relocating";
            case RELOCATED -> "relocated";
            case DRAINING -> "draining";
            case STOPPED -> "stopped";
            case ERROR -> "error";
        };
    }

    private static ZLinkFrameworkRuntimeState stateFromWire(String value) {
        return switch (value) {
            case "preparing" -> ZLinkFrameworkRuntimeState.PREPARING;
            case "serving" -> ZLinkFrameworkRuntimeState.SERVING;
            case "relocating" -> ZLinkFrameworkRuntimeState.RELOCATING;
            case "relocated" -> ZLinkFrameworkRuntimeState.RELOCATED;
            case "draining" -> ZLinkFrameworkRuntimeState.DRAINING;
            case "stopped" -> ZLinkFrameworkRuntimeState.STOPPED;
            case "error" -> ZLinkFrameworkRuntimeState.ERROR;
            default -> throw new IllegalStateException(
                "Unrecognized state: " + value);
        };
    }

    private static StoredDescriptor<ZLinkMeshNodeDescriptor> decodeMesh(
        byte[] bytes) {
        // No provider-internal generation is carried in the canonical
        // payload (21-location-runtime.md#2.4): the opaque record's own
        // store version already serves as the CAS fence, so this value is
        // an unused placeholder kept only for the shared update()/list()
        // scaffolding's signature.
        return new StoredDescriptor<>(0, decodeMeshNodeRecord(bytes));
    }

    private static StoredDescriptor<ZLinkClientServerServerDescriptor>
        decodeClientServer(byte[] bytes) {
        Envelope envelope = decodeEnvelope(bytes);
        return new StoredDescriptor<>(
            envelope.generation(),
            ZLinkLocationDescriptorCodec.deserializeClientServer(
                envelope.json(),
                0,
                extractUpdatedAt(envelope.json())));
    }

    private static StoredDescriptor<ZLinkFanoutPublisherDescriptor>
        decodeFanout(byte[] bytes) {
        Envelope envelope = decodeEnvelope(bytes);
        return new StoredDescriptor<>(
            envelope.generation(),
            ZLinkLocationDescriptorCodec.deserializeFanoutPublisher(
                envelope.json(),
                0,
                extractUpdatedAt(envelope.json())));
    }

    private static DescriptorIdentity identity(Object descriptor) {
        if (descriptor instanceof ZLinkMeshNodeDescriptor value) {
            return new DescriptorIdentity(
                value.ownerId(),
                value.leaseGeneration(),
                value.lifecycleGeneration(),
                value.descriptorRevision());
        }
        if (descriptor
            instanceof ZLinkClientServerServerDescriptor value) {
            return new DescriptorIdentity(
                value.ownerId(),
                value.leaseGeneration(),
                value.lifecycleGeneration(),
                value.descriptorRevision());
        }
        ZLinkFanoutPublisherDescriptor value =
            (ZLinkFanoutPublisherDescriptor) descriptor;
        return new DescriptorIdentity(
            value.ownerId(),
            value.leaseGeneration(),
            value.lifecycleGeneration(),
            value.descriptorRevision());
    }

    private static boolean sameImmutableMesh(
        ZLinkMeshNodeDescriptor current,
        ZLinkMeshNodeDescriptor next) {
        return current.meshName().equals(next.meshName())
            && current.rid().equals(next.rid())
            && current.lifecycleGeneration()
                == next.lifecycleGeneration()
            && current.endpoint().equals(next.endpoint())
            && current.channelWeights().keySet().equals(
                next.channelWeights().keySet())
            && current.applicationVersion()
                == next.applicationVersion()
            && current.objectCapabilities().equals(
                next.objectCapabilities())
            && current.objectRole() == next.objectRole()
            && current.entrySpotId().equals(next.entrySpotId())
            && current.capacity().actors().limit()
                == next.capacity().actors().limit()
            && current.capacity().spots().limit()
                == next.capacity().spots().limit()
            && current.securityIdentity().equals(
                next.securityIdentity())
            && current.ownerId().equals(next.ownerId())
            && current.leaseGeneration()
                == next.leaseGeneration();
    }

    private static byte[] encode(long generation, String json) {
        byte[] payload = json.getBytes(StandardCharsets.UTF_8);
        return ByteBuffer.allocate(Long.BYTES + payload.length)
            .putLong(generation)
            .put(payload)
            .array();
    }

    private static Envelope decodeEnvelope(byte[] bytes) {
        if (bytes.length <= Long.BYTES) {
            throw new IllegalStateException(
                "Location descriptor record is invalid");
        }
        ByteBuffer input = ByteBuffer.wrap(bytes);
        long generation = input.getLong();
        byte[] payload = new byte[input.remaining()];
        input.get(payload);
        if (generation <= 0) {
            throw new IllegalStateException(
                "Location descriptor generation is invalid");
        }
        return new Envelope(
            generation,
            new String(payload, StandardCharsets.UTF_8));
    }

    private static long extractLifecycle(String json) {
        try {
            return new ObjectMapper()
                .readTree(json)
                .path("LifecycleGeneration")
                .asLong();
        } catch (JsonProcessingException error) {
            throw new IllegalStateException(
                "Location descriptor record is invalid",
                error);
        }
    }

    private static Instant extractUpdatedAt(String json) {
        try {
            return Instant.parse(
                new ObjectMapper()
                    .readTree(json)
                    .path("UpdatedAt")
                    .asText());
        } catch (RuntimeException
            | JsonProcessingException error) {
            throw new IllegalStateException(
                "Location descriptor record is invalid",
                error);
        }
    }

    private static long decodeOwnerGeneration(byte[] bytes) {
        return ZLinkOwnerLeaseRecordCodec.decode(bytes).leaseGeneration();
    }

    private static String decodeOwnerId(byte[] bytes) {
        return ZLinkOwnerLeaseRecordCodec.decode(bytes).ownerId();
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
    private static ZLinkStoreKey meshKey(
        String meshName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            meshPrefix(meshName) + rid.toHex());
    }

    private static String meshPrefix(String meshName) {
        return "mesh-node\0" + requireNoNul(meshName, "meshName") + "\0";
    }

    private static ZLinkStoreKey clientServerKey(
        String channelName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            clientServerPrefix(channelName)
                + segment(rid.toHex()));
    }

    private static String clientServerPrefix(String channelName) {
        return PREFIX + "client-server:" + segment(channelName);
    }

    private static ZLinkStoreKey fanoutKey(
        String channelName,
        RoutingId rid) {
        return new ZLinkStoreKey(
            fanoutPrefix(channelName) + segment(rid.toHex()));
    }

    private static String fanoutPrefix(String channelName) {
        return PREFIX + "fanout:" + segment(channelName);
    }

    private static String segment(String value) {
        return value.getBytes(StandardCharsets.UTF_8).length
            + ":" + value + ":";
    }

    // A NUL-preimage segment's boundary is the NUL byte itself
    // (21-location-runtime.md#2.4), so the segment value must not contain
    // one.
    private static String requireNoNul(String value, String field) {
        if (value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                field + " must not contain a NUL byte");
        }
        return value;
    }

    private static String encodeContinuation(
        String prefix,
        ZLinkStoreScanCursor cursor) {
        byte[] cursorBytes =
            cursor.value().getBytes(StandardCharsets.UTF_8);
        if (cursorBytes.length < 1
            || cursorBytes.length > 4096) {
            throw new IllegalStateException(
                "Location Store returned an invalid snapshot cursor.");
        }
        return "v1." + base64(digest(prefix)) + "."
            + base64(cursorBytes);
    }

    private static ZLinkStoreScanCursor decodeContinuation(
        String prefix,
        String token) {
        if (token.length() < 1
            || token.length() > MAXIMUM_CONTINUATION_CHARACTERS) {
            throw invalidContinuation();
        }
        String[] parts = token.split("\\.", 3);
        if (parts.length != 3
            || !"v1".equals(parts[0])) {
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
            String value = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(cursor))
                .toString();
            return new ZLinkStoreScanCursor(value);
        } catch (IllegalArgumentException
            | CharacterCodingException error) {
            throw invalidContinuation();
        }
    }

    private static byte[] digest(String prefix) {
        try {
            return MessageDigest.getInstance("SHA-256")
                .digest(prefix.getBytes(StandardCharsets.UTF_8));
        } catch (NoSuchAlgorithmException error) {
            throw new IllegalStateException(
                "SHA-256 is required",
                error);
        }
    }

    private static String base64(byte[] bytes) {
        return Base64.getUrlEncoder().withoutPadding()
            .encodeToString(bytes);
    }

    private static byte[] decodeBase64(String value) {
        return Base64.getUrlDecoder().decode(value);
    }

    private static IllegalArgumentException invalidContinuation() {
        return new IllegalArgumentException(
            "Location Store continuation token is invalid.");
    }

    private static systems.zlink.framework.locationprovider
        .ZLinkStoreCancellation active() {
        return () -> false;
    }

    private static Throwable unwrap(Throwable failure) {
        return failure instanceof CompletionException completion
            && completion.getCause() != null
            ? completion.getCause()
            : failure;
    }

    private static <T> CompletionStage<T> completed(T value) {
        return CompletableFuture.completedFuture(value);
    }

    private record Envelope(long generation, String json) {}

    private record StoredDescriptor<T>(
        long generation,
        T descriptor) {}

    private record DescriptorIdentity(
        String ownerId,
        long leaseGeneration,
        long lifecycleGeneration,
        long descriptorRevision) {}
}
