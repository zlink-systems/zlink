package systems.zlink.framework.locations.redis;
import java.security.MessageDigest;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.concurrent.ExecutionException;
import java.util.stream.IntStream;
import systems.zlink.framework.locationprovider.ZLinkBlobConflict;
import systems.zlink.framework.locationprovider.ZLinkBlobFound;
import systems.zlink.framework.locationprovider.ZLinkBlobStored;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateAlreadyPrepared;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateParticipant;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepareRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregatePrepared;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateStale;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDelete;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityDeleted;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityExpectFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityGenerationTransition;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPut;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityScanExpired;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityStored;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkClientServerServerDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationIdentity;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationOperationTerminal;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalFound;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkCreationTerminalState;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkFanoutPublisherDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectAbortResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectConflict;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseReleaseResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityExhausted;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import io.lettuce.core.RedisClient;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.UUID;
import java.util.Arrays;
import java.util.stream.Collectors;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.locations.ZLinkPageRequest;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.spots.ZLinkSpotKind;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationDescriptorCodec;

class ZLinkRedisLocationStoreTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Instant UPDATED_AT = Instant.parse("2026-07-03T00:00:00Z");
    private static final RoutingId NODE_A = RoutingId.from(new byte[] {0x01});

    @Test
    void redisProviderExposesOnlyOpaqueCapability() {
        assertEquals(
            Set.of(
                systems.zlink.framework.locationprovider
                    .ZLinkLocationStore.class,
                AutoCloseable.class),
            Set.of(ZLinkRedisLocationStore.class.getInterfaces()));

        Set<String> publicMethods = Arrays.stream(
                ZLinkRedisLocationStore.class.getMethods())
            .map(Method::getName)
            .collect(Collectors.toSet());
        assertFalse(publicMethods.contains("updatePeer"));
        assertFalse(publicMethods.contains("resolveActor"));
        assertFalse(publicMethods.contains("resolveSpot"));
        assertFalse(publicMethods.contains("resolveRoute"));
        assertFalse(publicMethods.contains("getChangeStamp"));
        assertFalse(publicMethods.contains("getMeshNodeChangeStamp"));
        assertFalse(publicMethods.contains("reserve"));
        assertFalse(publicMethods.contains("prepareAggregate"));
        assertTrue(publicMethods.containsAll(
            Set.of("read", "write", "scan")));

        for (String removedType : List.of(
            "systems.zlink.framework.runtime.internal.locations.ZLinkPeerLocation",
            "systems.zlink.framework.runtime.internal.locations.ZLinkSpotLocation",
            "systems.zlink.framework.runtime.internal.locations.ZLinkActorLocation",
            "systems.zlink.framework.runtime.internal.locations.ZLinkRouteLocation",
            "systems.zlink.framework.runtime.internal.locations.ZLinkLocationChangeStampScope")) {
            assertThrows(
                ClassNotFoundException.class,
                () -> Class.forName(removedType));
        }
    }

    @Test
    void authorityPhysicalEncodingMatchesSharedFixture() throws Exception {
        JsonNode fixture = authorityFixture();
        JsonNode buckets = fixture.path("capacityBuckets");
        var descriptor = new ZLinkMeshNodeDescriptorKey(
                fixture.path("keyContract").path("meshName").asText(),
                RoutingId.fromHex("67616d652d61"));
        String descriptorKey =
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(descriptor);

        assertEquals(
            buckets.path("descriptorKey").asText(),
            descriptorKey);
        assertEquals(
            buckets.path("node").asText(),
            capacitySegment(descriptorKey)
                + capacitySegment(
                    buckets.path(
                        "descriptorLifecycleGeneration").asText()));
        assertEquals(
            buckets.path("type").asText(),
            capacityTypeBucket(
                descriptorKey,
                buckets.path(
                    "descriptorLifecycleGeneration").asText(),
                buckets.path("objectKind").asText(),
                buckets.path("stableType").asText()));
        assertEquals(
            buckets.path("unicodeType").asText(),
            capacityTypeBucket(
                descriptorKey,
                buckets.path(
                    "descriptorLifecycleGeneration").asText(),
                buckets.path("objectKind").asText(),
                buckets.path("unicodeStableType").asText()));
        assertEquals(
            List.of(
                "authorityKey",
                "payload",
                "storeVersion",
                "objectGeneration",
                "authorityOwnerGeneration",
                "ownerId",
                "ownerLeaseGeneration",
                "allocationState",
                "objectKind",
                "stableType",
                "descriptorKey",
                "descriptorLifecycleGeneration",
                "capacityDelta"),
            JSON.convertValue(
                fixture.path("currentHashFields"),
                JSON.getTypeFactory().constructCollectionType(
                    List.class,
                    String.class)));
    }

    @Test
    void meshNodeDescriptorPhysicalEncodingMatchesSharedFixture()
        throws Exception {
        JsonNode fixture = descriptorFixture();
        JsonNode physical = fixture.path("physicalKeys");
        JsonNode row = fixture.path("row");
        String canonicalKey = row.path("key").asText();
        var keys = new ZLinkRedisLocationKeys("P");
        var descriptorKey =
            new ZLinkMeshNodeDescriptorKey(
                    row.path("hash").path("mesh").asText(),
                    RoutingId.fromHex("67616d652d61"));

        assertEquals(
            canonicalKey,
            ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(
                descriptorKey));
        assertEquals(
            physical.path("descriptor").asText(),
            keys.meshNodeDescriptorRowKey(descriptorKey));
        assertEquals(
            physical.path("admission").asText(),
            keys.meshNodeDescriptorMetadataKey(descriptorKey));
        assertEquals(
            physical.path("descriptorIndex").asText(),
            keys.kindIndexKey("mesh-node"));
        assertEquals(
            physical.path("descriptorOwnerIndex").asText(),
            keys.meshNodeOwnerTokenIndexKey(
                "mesh-owner-a",
                9));
        assertEquals(
            physical.path("ownerLease").asText(),
            keys.leaseKey("mesh-owner-a"));
        JsonNode entryClaim = fixture.path("entrySpotIdentityClaim");
        assertEquals(
            physical.path("entrySpotIdentityClaim").asText(),
            keys.entrySpotIdentityClaimKey(
                entryClaim.path("hash").path("spotId").asText()));
        assertEquals(
            Set.of(
                "state",
                "spotId",
                "descriptorKey",
                "descriptorLifecycleGeneration",
                "ownerId",
                "ownerLeaseGeneration"),
            Set.copyOf(JSON.convertValue(
                entryClaim.path("hashFields"),
                JSON.getTypeFactory().constructCollectionType(
                    List.class,
                    String.class))));
    }

    @Test
    void clientServerDescriptorPhysicalEncodingMatchesSharedFixture()
        throws Exception {
        JsonNode fixture = redisFixture(
            "client-server-server-descriptor-v1.json");
        JsonNode row = fixture.path("row");
        JsonNode hash = row.path("hash");
        var descriptor =
            new ZLinkClientServerServerDescriptor(
                    "orders",
                    RoutingId.from("orders-a"),
                    7,
                    3,
                    "tcp://10.0.0.2:7400",
                    100,
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING,
                    "cluster-a",
                    "channel-owner-a",
                    5,
                    Instant.parse(
                        "2024-07-15T00:00:00Z"));
        assertEquals(
            row.path("key").asText(),
            ZLinkRedisLocationKeyCodec.encodeClientServerKey(
                new ZLinkClientServerServerDescriptorKey(
                        descriptor.channelName(),
                        descriptor.serverRid())));
        assertEquals(
            hash.path("json").asText(),
            ZLinkLocationDescriptorCodec.serializeClientServer(
                descriptor));
    }

    @Test
    void fanoutPublisherPhysicalEncodingMatchesSharedFixture()
        throws Exception {
        JsonNode fixture = redisFixture(
            "fanout-publisher-descriptor-v1.json");
        JsonNode row = fixture.path("row");
        JsonNode hash = row.path("hash");
        var descriptor =
            new ZLinkFanoutPublisherDescriptor(
                    "events",
                    RoutingId.from("events-pub-a"),
                    7,
                    3,
                    "tcp://10.0.0.3:7500",
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING,
                    "cluster-a",
                    "fanout-owner-a",
                    5,
                    Instant.parse("2024-07-15T00:00:00Z"));
        assertEquals(
            row.path("key").asText(),
            ZLinkRedisLocationKeyCodec.encodeFanoutPublisherKey(
                new ZLinkFanoutPublisherDescriptorKey(
                        descriptor.channelName(),
                        descriptor.publisherRid())));
        assertEquals(
            hash.path("json").asText(),
            ZLinkLocationDescriptorCodec.serializeFanoutPublisher(
                descriptor));
    }

    @Test
    void fanoutPublisherListExcludesReleasedOwnerLease()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(
                        "zlink:fanout-lease-test:"
                            + UUID.randomUUID()))) {
            ZLinkLocationOwnerToken owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "fanout-owner",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get())
                .token();
            var descriptor =
                new ZLinkFanoutPublisherDescriptor(
                        "events",
                        RoutingId.from("publisher"),
                        7,
                        1,
                        "tcp://127.0.0.1:7500",
                        systems.zlink.framework.runtime.host
                            .ZLinkFrameworkRuntimeState.SERVING,
                        "default",
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        Instant.now());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateFanoutPublisher(
                        descriptor,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get()
                    .status());
            assertEquals(
                1,
                store.listFanoutPublishers(
                        "events",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().size());

            store.releaseOwnerLease(owner)
                .toCompletableFuture().get();

            assertTrue(
                store.listFanoutPublishers(
                        "events",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().isEmpty());
        }
    }

    @Test
    void redisLocationSchemaGateRejectsIncompatibleMarker()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        String prefix =
            "zlink:schema-gate-test:" + UUID.randomUUID();
        var options = new ZLinkRedisLocationOptions()
            .setConnectionString(endpoint)
            .setKeyPrefix(prefix);
        RedisClient client = RedisClient.create(options.redisUri());
        try (var connection = client.connect()) {
            connection.sync().hset(
                new ZLinkRedisLocationKeys(prefix).schemaKey(),
                Map.of(
                    "format", "different-schema",
                    "epoch", "9"));
        } finally {
            client.shutdown();
        }
        try (var store = new ZLinkRedisLocationRepository(options)) {
            var failure = assertThrows(
                ExecutionException.class,
                () -> store.read(
                        "zla1:a:schema-gate",
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                IllegalStateException.class,
                failure.getCause());
        }
    }

    @Test
    void redisRelocationStoreKeepsCallerIssuedReferenceAndImmutablePayload()
        throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisRelocationStore store = new ZLinkRedisRelocationStore(
            new ZLinkRedisRelocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix("zlink:relocation-test:" + UUID.randomUUID()))) {
            byte[] payload = new byte[] {1, 2, 3, 4};
            var reference = new systems.zlink.framework.locationprovider
                .ZLinkBlobReference(UUID.randomUUID().toString());
            var stored = assertInstanceOf(
                ZLinkBlobStored.class,
                store.put(
                    reference,
                    payload,
                    Duration.ofHours(24),
                    () -> false)
                .toCompletableFuture().get());
            payload[0] = 9;

            assertInstanceOf(
                systems.zlink.framework.locationprovider
                    .ZLinkBlobAlreadyStored.class,
                store.put(
                    reference,
                    new byte[] {1, 2, 3, 4},
                    Duration.ofHours(24),
                    () -> false).toCompletableFuture().get());
            assertInstanceOf(
                ZLinkBlobConflict.class,
                store.put(
                    reference,
                    new byte[] {4, 3, 2, 1},
                    Duration.ofHours(24),
                    () -> false).toCompletableFuture().get());
            var found = assertInstanceOf(
                ZLinkBlobFound.class,
                store.read(reference, () -> false)
                    .toCompletableFuture().get());
            assertArrayEquals(new byte[] {1, 2, 3, 4}, found.bytes());
            assertTrue(stored.expiresAt().isAfter(stored.storeNow()));
            store.delete(reference, () -> false).toCompletableFuture().get();
            store.delete(reference, () -> false).toCompletableFuture().get();
        }
    }

    @Test
    void redisAuthorityReservePreserveScanAndDeleteAreFenced() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        String keyPrefix =
            "zlink:authority-test:" + UUID.randomUUID();
        try (ZLinkRedisLocationRepository store = new ZLinkRedisLocationRepository(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix(keyPrefix))) {
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        descriptor(
                            NODE_A,
                            1,
                            1,
                            owner,
                            "player",
                            8,
                            4),
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            var request = new ZLinkObjectReservationRequest(
                    ZLinkPlacementObjectKind.ACTOR,
                    "zla1:a:4:game:7:actor-1",
                    "player",
                    "creation-root",
                    new byte[32],
                    32,
                    new ZLinkMeshNodeDescriptorKey(
                        "game",
                        NODE_A),
                    1,
                    owner,
                    new byte[] {9, 8},
                    ZLinkPlacementCapacityBundle.actor(1));
            var reserved = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(request, () -> false).toCompletableFuture().get());
            assertInstanceOf(
                ZLinkObjectConflict.class,
                store.reserve(request, () -> false)
                    .toCompletableFuture().get());
            var creating = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(request.authorityKey(), () -> false)
                    .toCompletableFuture().get());
            var physicalKeys = new ZLinkRedisLocationKeys(keyPrefix);
            RedisClient inspectionClient = RedisClient.create(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(keyPrefix)
                    .redisUri());
            try (var inspection = inspectionClient.connect()) {
                var redis = inspection.sync();
                assertEquals(
                    Map.of(
                        "format", "location-authority-hybrid-v1",
                        "epoch", "1"),
                    redis.hgetall(physicalKeys.schemaKey()));
                assertEquals(
                    Set.of("ownerId", "generation", "expiresAt"),
                    Set.copyOf(redis.hkeys(
                        physicalKeys.leaseKey(owner.ownerId()))));
                assertEquals(
                    0,
                    redis.exists(
                        physicalKeys.legacyLeaseKey(
                            owner.ownerId())));
                assertEquals(
                    Set.of(
                        "owner", "gen", "json",
                        "updatedAtMs", "mesh"),
                    Set.copyOf(redis.hkeys(
                        physicalKeys.meshNodeDescriptorRowKey(
                            request.targetDescriptor()))));
                assertTrue(redis.exists(
                    physicalKeys.meshNodeDescriptorMetadataKey(
                        request.targetDescriptor())) > 0);
                assertEquals(
                    Set.copyOf(JSON.convertValue(
                        descriptorFixture().path(
                            "admissionHashFields"),
                        JSON.getTypeFactory()
                            .constructCollectionType(
                                List.class,
                                String.class))),
                    Set.copyOf(redis.hkeys(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()))));
                assertEquals(
                    ZLinkRedisLocationKeyCodec
                        .encodeMeshNodeKey(
                            request.targetDescriptor()),
                    redis.hget(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()),
                        "descriptorKey"));
                assertEquals(
                    owner.ownerId(),
                    redis.hget(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()),
                        "ownerId"));
                assertEquals(
                    Long.toString(owner.leaseGeneration()),
                    redis.hget(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()),
                        "ownerLeaseGeneration"));
                assertEquals(
                    "server",
                    redis.hget(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()),
                        "objectRole"));
                assertEquals(
                    "1",
                    redis.hget(
                        physicalKeys
                            .meshNodeDescriptorMetadataKey(
                                request.targetDescriptor()),
                        "runtimeState"));
                assertEquals(
                    Set.of(
                        ZLinkRedisLocationKeyCodec
                            .encodeMeshNodeKey(
                                request.targetDescriptor())),
                    redis.smembers(
                        physicalKeys
                            .meshNodeOwnerTokenIndexKey(
                                owner.ownerId(),
                                owner.leaseGeneration())));
                assertTrue(redis.exists(
                    physicalKeys.authorityRowKey(
                        request.authorityKey())) > 0);
                Set<String> expectedAuthorityFields =
                    new HashSet<>(JSON.convertValue(
                        authorityFixture().path(
                            "currentHashFields"),
                        JSON.getTypeFactory()
                            .constructCollectionType(
                                List.class,
                                String.class)));
                expectedAuthorityFields.addAll(Set.of(
                    "creationReservationId",
                    "creationIntentReference",
                    "creationIntentSha256",
                    "creationIntentEncodedSize"));
                expectedAuthorityFields.remove("capacityDelta");
                expectedAuthorityFields.add("capacityBundle");
                assertEquals(
                    expectedAuthorityFields,
                    Set.copyOf(redis.hkeys(
                        physicalKeys.authorityRowKey(
                            request.authorityKey()))));
                assertEquals(
                    "actor",
                    redis.hget(
                        physicalKeys.authorityRowKey(
                            request.authorityKey()),
                        "objectKind"));
                assertTrue(redis.exists(
                    physicalKeys.creationKey(
                        reserved.reservation()
                            .reservationVersion())) > 0);
                assertEquals(
                    0,
                    redis.exists(
                        physicalKeys.capacityTypePendingKey()));
                assertTrue(redis.exists(
                    physicalKeys.capacityNodePendingKey()) > 0);
                String descriptorIdentity =
                    ZLinkRedisLocationKeyCodec.encodeMeshNodeKey(
                        request.targetDescriptor());
                String nodeBucket =
                    capacitySegment(descriptorIdentity)
                        + capacitySegment(Long.toString(
                            request
                                .targetDescriptorLifecycleGeneration()))
                        + capacitySegment("actor");
                String typeBucket =
                    capacitySegment(descriptorIdentity)
                    + capacitySegment(Long.toString(
                        request
                            .targetDescriptorLifecycleGeneration()))
                    + capacitySegment("actor")
                    + capacitySegment(request.stableType());
                assertEquals(
                    "1",
                    redis.hget(
                        physicalKeys.capacityNodePendingKey(),
                        nodeBucket));
                assertNull(redis.hget(
                    physicalKeys.capacityTypePendingKey(),
                    typeBucket));
            } finally {
                inspectionClient.shutdown();
            }
            assertArrayEquals(new byte[] {9, 8}, creating.payload());
            assertEquals(owner.ownerId(), creating.ownerId());
            assertEquals(
                owner.leaseGeneration(),
                creating.ownerLeaseGeneration());
            assertEquals(
                ZLinkPlacementAllocationState.PENDING,
                creating.allocation().state());
            assertEquals(
                request.targetDescriptor(),
                creating.allocation().descriptor());
            assertEquals(
                request.capacityBundle(),
                creating.allocation().capacityBundle());
            var pendingCreation = creating.pendingCreation().orElseThrow();
            assertEquals(
                reserved.reservation().reservationVersion(),
                pendingCreation.reservationId());
            assertEquals(
                request.creationIntentReference(),
                pendingCreation.requestContentReference());
            assertArrayEquals(
                request.creationIntentHash(),
                pendingCreation.requestSha256());
            assertEquals(
                request.creationIntentEncodedSize(),
                pendingCreation.requestEncodedSize());
            byte[] terminalEnvelope =
                "creation-operation-terminal-v1:created"
                    .getBytes(StandardCharsets.UTF_8);
            byte[] terminalSha256 = MessageDigest
                .getInstance("SHA-256")
                .digest(terminalEnvelope);
            var creationOperation =
                new ZLinkCreationOperationIdentity(
                        NODE_A,
                        1,
                        0x11,
                        0x22);
            var terminal =
                new ZLinkCreationOperationTerminal(
                        creationOperation,
                        reserved.reservation(),
                        ZLinkCreationTerminalState.CREATED,
                        terminalEnvelope,
                        terminalSha256,
                        Instant.now().plus(Duration.ofMinutes(5)));
            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(
                        reserved.reservation(),
                        new byte[] {1, 2},
                        terminal,
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectCommitResult.ALREADY_COMMITTED,
                store.commit(
                        reserved.reservation(),
                        new byte[] {1, 2},
                        terminal,
                        () -> false)
                    .toCompletableFuture().get());
            var storedTerminal = assertInstanceOf(
                ZLinkCreationTerminalFound.class,
                store.readCreationTerminal(
                        creationOperation,
                        () -> false)
                    .toCompletableFuture().get())
                .terminal();
            assertEquals(
                ZLinkCreationTerminalState.CREATED,
                storedTerminal.state());
            assertArrayEquals(
                terminalEnvelope,
                storedTerminal.terminalEnvelope());
            assertInstanceOf(
                ZLinkCreationTerminalMissing.class,
                store.readCreationTerminal(
                        new ZLinkCreationOperationIdentity(
                                NODE_A,
                                1,
                                0x11,
                                0x23),
                        () -> false)
                    .toCompletableFuture().get());
            RedisClient postCommitClient = RedisClient.create(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(keyPrefix)
                    .redisUri());
            try (var inspection = postCommitClient.connect()) {
                var redis = inspection.sync();
                assertEquals(
                    0,
                    redis.exists(physicalKeys.creationKey(
                        reserved.reservation()
                            .reservationVersion())));
                assertEquals(
                    0,
                    redis.exists(
                        physicalKeys.capacityTypeActiveKey()));
                assertTrue(redis.exists(
                    physicalKeys.capacityNodeActiveKey()) > 0);
            } finally {
                postCommitClient.shutdown();
            }
            var staleReservation =
                new ZLinkObjectReservation(
                        reserved.reservation().authorityKey(),
                        reserved.reservation().storeVersion(),
                        reserved.reservation().objectGeneration(),
                        reserved.reservation()
                            .authorityOwnerGeneration(),
                        reserved.reservation().reservationVersion(),
                        reserved.reservation().targetDescriptor(),
                        reserved.reservation()
                            .targetDescriptorLifecycleGeneration() + 1,
                        reserved.reservation().targetOwner());
            assertEquals(
                ZLinkObjectCommitResult.STALE,
                store.commit(
                        staleReservation,
                        new byte[] {1, 2},
                        () -> false)
                    .toCompletableFuture().get());
            var current = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(request.authorityKey(), () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkPlacementAllocationState.ACTIVE,
                current.allocation().state());
            assertTrue(current.pendingCreation().isEmpty());
            var updated = assertInstanceOf(
                ZLinkAuthorityStored.class,
                store.compareExchange(
                        request.authorityKey(),
                        new ZLinkAuthorityExpectFound(
                            current.storeVersion()),
                        new ZLinkAuthorityPut(new byte[] {3, 4}),
                        () -> false)
                    .toCompletableFuture().get());
            assertArrayEquals(new byte[] {3, 4}, updated.payload());
            assertEquals(current.allocation(), updated.allocation());
            String secondKey = "zla1:a:4:game:7:actor-2";
            var secondReservation = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            secondKey,
                            descriptor(
                                NODE_A,
                                1,
                                1,
                                owner,
                                "player",
                                8,
                                4),
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(
                        secondReservation.reservation(),
                        new byte[] {5},
                        () -> false)
                    .toCompletableFuture().get());
            var page = assertInstanceOf(
                ZLinkAuthorityPage.class,
                store.list("zla1:a:", Optional.empty(), 1, () -> false)
                    .toCompletableFuture().get());
            assertEquals(List.of(request.authorityKey()),
                page.items().stream()
                    .map(ZLinkAuthorityEntry::key)
                    .toList());
            var secondCurrent = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(secondKey, () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityStored.class,
                store.compareExchange(
                        secondKey,
                        new ZLinkAuthorityExpectFound(
                                secondCurrent.storeVersion()),
                        new ZLinkAuthorityPut(new byte[] {6}),
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                store.compareExchange(
                        request.authorityKey(),
                        new ZLinkAuthorityExpectFound(
                            updated.storeVersion()),
                        new ZLinkAuthorityDelete(),
                        () -> false)
                    .toCompletableFuture().get());
            String postWatermarkKey =
                "zla1:a:4:game:7:actor-3";
            var postWatermark = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            postWatermarkKey,
                            descriptor(
                                NODE_A,
                                1,
                                1,
                                owner,
                                "player",
                                8,
                                4),
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(
                        postWatermark.reservation(),
                        new byte[] {7},
                        () -> false)
                    .toCompletableFuture().get());
            var secondPage = assertInstanceOf(
                ZLinkAuthorityPage.class,
                store.list(
                        "zla1:a:",
                        page.nextCursor(),
                        10,
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(List.of(secondKey),
                secondPage.items().stream()
                    .map(ZLinkAuthorityEntry::key)
                    .toList());
            assertArrayEquals(
                new byte[] {5},
                secondPage.items().getFirst().snapshot().payload());
            assertTrue(secondPage.nextCursor().isEmpty());
            assertInstanceOf(
                ZLinkAuthorityScanExpired.class,
                store.list(
                        "zla1:a:",
                        page.nextCursor(),
                        10,
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityPage.class,
                store.list(
                        "zla1:a:",
                        Optional.empty(),
                        100,
                        () -> false)
                    .toCompletableFuture().get());
            RedisClient gcInspectionClient = RedisClient.create(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(keyPrefix)
                    .redisUri());
            try (var inspection = gcInspectionClient.connect()) {
                var redis = inspection.sync();
                assertEquals(
                    null,
                    redis.zscore(
                        physicalKeys.authorityIndexKey(),
                        physicalKeys.encodedAuthorityKey(
                            request.authorityKey())));
                assertTrue(
                    redis.zcard(
                        physicalKeys
                            .authorityHistoryRevisionsKey(
                                request.authorityKey()))
                        <= 2);
            } finally {
                gcInspectionClient.shutdown();
            }
        }
    }

    @Test
    void redisMeshNodeDescriptorAndCreationAdmissionAreFailClosed()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(
                        "zlink:descriptor-admission-test:"
                            + UUID.randomUUID()))) {
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "descriptor-owner",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var initial = descriptor(
                NODE_A,
                11,
                1,
                owner,
                "player",
                2,
                1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        initial,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        initial,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());
            assertEquals(
                Set.of(
                    "owner",
                    "gen",
                    "json",
                    "updatedAtMs",
                    "mesh"),
                store.readMeshNodeHashFields(
                        new ZLinkMeshNodeDescriptorKey(
                                "game",
                                NODE_A))
                    .toCompletableFuture().get().keySet());
            var sameRevisionDifferentBytes = descriptor(
                NODE_A,
                11,
                1,
                owner,
                "different-type",
                2,
                1);
            var protocolError = assertThrows(
                ExecutionException.class,
                () -> store.updateMeshNode(
                        sameRevisionDifferentBytes,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get());
            assertInstanceOf(
                IllegalStateException.class,
                protocolError.getCause());
            var page = store.listMeshNodes(
                    "game",
                    ZLinkPageRequest.firstPage())
                .toCompletableFuture().get();
            var storedDescriptor = page.items().getFirst();
            assertEquals(initial.meshName(), storedDescriptor.meshName());
            assertEquals(
                initial.objectCapabilities(),
                storedDescriptor.objectCapabilities());
            assertEquals(
                initial.capacity(),
                storedDescriptor.capacity());

            var mutableUpdate = descriptor(
                NODE_A,
                11,
                2,
                owner,
                "player",
                2,
                1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        mutableUpdate,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());
            var invalidImmutableUpdate = descriptor(
                NODE_A,
                11,
                3,
                owner,
                "different-type",
                3,
                1);
            assertEquals(
                ZLinkLocationWriteStatus.IGNORED_STALE,
                store.updateMeshNode(
                        invalidImmutableUpdate,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());
            String firstKey = "zla1:a:descriptor-capacity-1";
            var first = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            firstKey,
                            mutableUpdate,
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get());
            var second = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            "zla1:a:descriptor-capacity-2",
                            mutableUpdate,
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                store.reserve(
                        reservationRequest(
                            "zla1:a:descriptor-capacity-3",
                            mutableUpdate,
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectAbortResult.ABORTED,
                store.abort(second.reservation(), () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkObjectConflict.class,
                store.reserve(
                        reservationRequest(
                            "zla1:a:descriptor-unsupported-type",
                            mutableUpdate,
                            owner,
                            "unsupported"),
                        () -> false)
                    .toCompletableFuture().get());

            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey(
                                "game",
                                NODE_A),
                        owner)
                    .toCompletableFuture().get());
            var replacement = descriptor(
                NODE_A,
                12,
                1,
                owner,
                "player",
                3,
                1);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        replacement,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            assertEquals(
                ZLinkObjectCommitResult.STALE,
                store.commit(
                        first.reservation(),
                        new byte[] {2},
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkObjectAbortResult.ABORTED,
                store.abort(first.reservation(), () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                1,
                store.removeAllByOwner(owner)
                    .toCompletableFuture().get());
            assertEquals(
                List.of(),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get().items());
        }
    }

    @Test
    void redisEntrySpotIdentityClaimUsesExactOwnerCleanup()
        throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        String keyPrefix =
            "zlink:entry-identity-test:" + UUID.randomUUID();
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(keyPrefix))) {
            var firstOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease("entry-owner-a", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var secondOwner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease("entry-owner-b", Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            String entrySpotId =
                "game-entry-00000000-0000-4000-8000-000000000099";
            var first = descriptor(
                NODE_A, 41, 1, firstOwner, "player", 2, 1,
                entrySpotId);
            RoutingId secondRid = RoutingId.from(new byte[] {0x02});
            var second = descriptor(
                secondRid, 42, 1, secondOwner, "player", 2, 1,
                entrySpotId);

            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        first,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            var physicalKeys = new ZLinkRedisLocationKeys(keyPrefix);
            RedisClient inspectionClient = RedisClient.create(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(keyPrefix)
                    .redisUri());
            try (var inspection = inspectionClient.connect()) {
                var redis = inspection.sync();
                String claimKey =
                    physicalKeys.entrySpotIdentityClaimKey(entrySpotId);
                assertEquals(
                    Set.of(
                        "state",
                        "spotId",
                        "descriptorKey",
                        "descriptorLifecycleGeneration",
                        "ownerId",
                        "ownerLeaseGeneration"),
                    Set.copyOf(redis.hkeys(claimKey)));
                assertEquals("Claimed", redis.hget(claimKey, "state"));
                assertEquals(entrySpotId, redis.hget(claimKey, "spotId"));
                assertEquals(-1, redis.pttl(claimKey));
            } finally {
                inspectionClient.shutdown();
            }
            String entryAuthorityKey =
                systems.zlink.framework.runtime.locations
                    .ZLinkAuthorityKeyCodec.spot(entrySpotId);
            assertInstanceOf(
                ZLinkObjectConflict.class,
                store.reserve(
                        new ZLinkObjectReservationRequest(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                entryAuthorityKey,
                                "player",
                                "creation-root",
                                new byte[32],
                                32,
                                new ZLinkMeshNodeDescriptorKey(
                                        first.meshName(),
                                        first.rid()),
                                first.lifecycleGeneration(),
                                firstOwner,
                                new byte[] {1},
                                ZLinkPlacementCapacityBundle.spot(
                                        ZLinkPlacementObjectKind.USER_SPOT,
                                        "player",
                                        1)),
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityMissing.class,
                store.read(entryAuthorityKey, () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkLocationWriteStatus.REJECTED_CONFLICT,
                store.updateMeshNode(
                        second,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey("game", NODE_A),
                        firstOwner)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        second,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());
            assertEquals(
                ZLinkLocationWriteStatus.IGNORED_STALE,
                store.removeMeshNode(
                        new ZLinkMeshNodeDescriptorKey("game", NODE_A),
                        firstOwner)
                    .toCompletableFuture().get());
        }
    }

    @Test
    void redisClientServerDescriptorUsesDedicatedFencedStore()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(
                        "zlink:client-server-descriptor-test:"
                            + UUID.randomUUID()))) {
            ZLinkLocationOwnerToken owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "client-server-owner",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            RoutingId serverRid = RoutingId.from("orders-a");
            var initial =
                new ZLinkClientServerServerDescriptor(
                        "orders",
                        serverRid,
                        7,
                        1,
                        "tcp://127.0.0.1:7400",
                        100,
                        systems.zlink.framework.runtime.host
                            .ZLinkFrameworkRuntimeState.SERVING,
                        "cluster-a",
                        owner.ownerId(),
                        owner.leaseGeneration(),
                        UPDATED_AT);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateClientServer(
                        initial,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());

            var changed = new ZLinkClientServerServerDescriptor(
                    initial.channelName(),
                    initial.serverRid(),
                    initial.lifecycleGeneration(),
                    2,
                    initial.endpoint(),
                    25,
                    initial.state(),
                    initial.securityIdentity(),
                    initial.ownerId(),
                    initial.leaseGeneration(),
                    initial.updatedAt());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateClientServer(
                        changed,
                        ZLinkLocationWriteIntent.RENEW)
                    .toCompletableFuture().get().status());

            var page = store.listClientServers(
                    "orders",
                    new ZLinkPageRequest(1, null))
                .toCompletableFuture().get();
            assertEquals(1, page.items().size());
            assertEquals(
                changed.channelName(),
                page.items().getFirst().channelName());
            assertEquals(
                changed.serverRid(),
                page.items().getFirst().serverRid());
            assertEquals(
                changed.descriptorRevision(),
                page.items().getFirst().descriptorRevision());
            assertEquals(
                changed.weight(),
                page.items().getFirst().weight());
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.removeClientServer(
                        new ZLinkClientServerServerDescriptorKey(
                                "orders",
                                serverRid),
                        owner)
                    .toCompletableFuture().get());
            assertEquals(
                List.of(),
                store.listClientServers(
                        "orders",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get().items());
        }
    }

    @Test
    void redisMeshNodeCapacityProjectionTracksAuthorityTransactions()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(
                        "zlink:capacity-projection-test:"
                            + UUID.randomUUID()))) {
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "capacity-owner",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var meshNode = descriptor(
                NODE_A,
                31,
                1,
                owner,
                "player",
                8,
                8);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        meshNode,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());

            String authorityKey =
                "zla1:a:capacity-projection";
            var reserved = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        reservationRequest(
                            authorityKey,
                            meshNode,
                            owner,
                            "player"),
                        () -> false)
                    .toCompletableFuture().get()).reservation();
            assertEquals(
                actorCapacity(0, 1, 8),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().getFirst().capacity());

            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(
                        reserved,
                        new byte[] {2},
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                actorCapacity(1, 0, 8),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().getFirst().capacity());

            var active = assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                store.read(authorityKey, () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                store.compareExchange(
                        authorityKey,
                        new ZLinkAuthorityExpectFound(
                                active.storeVersion()),
                        new ZLinkAuthorityDelete(),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                actorCapacity(0, 0, 8),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().getFirst().capacity());
        }
    }

    @Test
    void redisProjectsUserSpotPopulationAndStableTypeUsage()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(
                new ZLinkRedisLocationOptions()
                    .setConnectionString(endpoint)
                    .setKeyPrefix(
                        "zlink:spot-capacity-projection-test:"
                            + UUID.randomUUID()))) {
            var owner = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "spot-capacity-owner",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var meshNode = new ZLinkMeshNodeDescriptor(
                    "game",
                    NODE_A,
                    32,
                    1,
                    "tcp://127.0.0.1:7000",
                    Map.of(),
                    1,
                    List.of(new ZLinkObjectCapability(
                            ZLinkPlacementObjectKind.USER_SPOT,
                            "lobby",
                            ZLinkObjectMaintenancePolicyKind
                                .SNAPSHOT,
                            true,
                            2)),
                    ZLinkMeshNodeObjectRole.SERVER,
                    Optional.of(
                        "game-entry-00000000-0000-4000-8000-000000000032"),
                    100,
                    spotCapacity(0, 0, 3, 0, 0, 2),
                    new ZLinkActivationConcurrency(0, 128),
                    Optional.empty(),
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRuntimeState.SERVING,
                    "security",
                    owner.ownerId(),
                    owner.leaseGeneration(),
                    UPDATED_AT);
            assertEquals(
                ZLinkLocationWriteStatus.STORED,
                store.updateMeshNode(
                        meshNode,
                        ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture().get().status());

            var reserved = assertInstanceOf(
                ZLinkObjectReserved.class,
                store.reserve(
                        new ZLinkObjectReservationRequest(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                systems.zlink.framework.runtime.locations
                                    .ZLinkAuthorityKeyCodec.spot("lobby-1"),
                                "lobby",
                                "creation-root",
                                new byte[32],
                                32,
                                new ZLinkMeshNodeDescriptorKey(
                                        "game", NODE_A),
                                32,
                                owner,
                                new byte[] {1},
                                ZLinkPlacementCapacityBundle.spot(
                                        ZLinkPlacementObjectKind.USER_SPOT,
                                        "lobby",
                                        1)),
                        () -> false)
                    .toCompletableFuture().get()).reservation();
            assertEquals(
                spotCapacity(0, 1, 3, 0, 1, 2),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().getFirst().capacity());

            assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                store.commit(reserved, new byte[] {2}, () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                spotCapacity(1, 0, 3, 1, 0, 2),
                store.listMeshNodes(
                        "game",
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get()
                    .items().getFirst().capacity());
        }
    }

    @Test
    void redisAggregatePersistsExactPrepareAndFinalizesCapacityAtomically()
        throws Exception {
        String endpoint =
            System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        String prefix = "zlink:aggregate-test:" + UUID.randomUUID();
        var options = new ZLinkRedisLocationOptions()
            .setConnectionString(endpoint)
            .setKeyPrefix(prefix);
        try (ZLinkRedisLocationRepository store =
            new ZLinkRedisLocationRepository(options)) {
            var source = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "aggregate-source",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            var target = assertInstanceOf(
                ZLinkOwnerLeaseClaimed.class,
                store.claimOwnerLease(
                        "aggregate-target",
                        Duration.ofSeconds(30))
                    .toCompletableFuture().get()).token();
            RoutingId sourceRid =
                RoutingId.from(new byte[] {0x11});
            RoutingId targetRid =
                RoutingId.from(new byte[] {0x12});
            var sourceDescriptor = descriptor(
                sourceRid,
                21,
                1,
                source,
                "player",
                8,
                4);
            var targetDescriptor = descriptor(
                targetRid,
                22,
                1,
                target,
                "player",
                8,
                4);
            store.updateMeshNode(
                    sourceDescriptor,
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();
            store.updateMeshNode(
                    targetDescriptor,
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();

            List<String> authorityKeys = List.of(
                "zla1:a:aggregate-a",
                "zla1:a:aggregate-b");
            List<ZLinkAuthoritySnapshot> snapshots =
                new ArrayList<>();
            for (String authorityKey : authorityKeys) {
                var reservation = assertInstanceOf(
                    ZLinkObjectReserved.class,
                    store.reserve(
                            reservationRequest(
                                authorityKey,
                                sourceDescriptor,
                                source,
                                "player"),
                            () -> false)
                        .toCompletableFuture().get());
                assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    store.commit(
                            reservation.reservation(),
                            new byte[] {1},
                            () -> false)
                        .toCompletableFuture().get());
                snapshots.add(assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    store.read(authorityKey, () -> false)
                        .toCompletableFuture().get()));
            }

            UUID aggregateId = UUID.randomUUID();
            var participants =
                IntStream.range(
                        0,
                        authorityKeys.size())
                    .mapToObj(index ->
                        new ZLinkAggregateParticipant(
                                authorityKeys.get(index),
                                snapshots.get(index).objectGeneration(),
                                snapshots.get(index)
                                    .authorityOwnerGeneration(),
                                snapshots.get(index).storeVersion(),
                                ZLinkAuthorityGenerationTransition
                                    .NEW_OWNER,
                                new byte[] {(byte) (3 + index)},
                                new byte[] {(byte) (5 + index)}))
                    .toList();
            var prepareRequest =
                new ZLinkAggregatePrepareRequest(
                        aggregateId,
                        1,
                        participants,
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey(
                                "game",
                                targetRid),
                        22,
                        ZLinkPlacementCapacityBundle.actor(
                                authorityKeys.size()),
                        target);
            var prepared = assertInstanceOf(
                ZLinkAggregatePrepared.class,
                store.prepareAggregate(
                        prepareRequest,
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAggregateAlreadyPrepared.class,
                store.prepareAggregate(
                        prepareRequest,
                        () -> false)
                    .toCompletableFuture().get());
            var changed = new ArrayList<>(participants);
            changed.set(
                0,
                new ZLinkAggregateParticipant(
                        participants.getFirst().authorityKey(),
                        participants.getFirst().objectGeneration(),
                        participants.getFirst()
                            .sourceAuthorityOwnerGeneration(),
                        participants.getFirst().expectedStoreVersion(),
                        participants.getFirst().ownerTransition(),
                        new byte[] {99},
                        participants.getFirst()
                            .membershipMutation()));
            assertInstanceOf(
                ZLinkAggregateConflict.class,
                store.prepareAggregate(
                        new ZLinkAggregatePrepareRequest(
                                aggregateId,
                                1,
                                changed,
                                new byte[32],
                                new ZLinkMeshNodeDescriptorKey(
                                        "game",
                                        targetRid),
                                22,
                                ZLinkPlacementCapacityBundle.actor(
                                        authorityKeys.size()),
                                target),
                        () -> false)
                    .toCompletableFuture().get());
            assertInstanceOf(
                ZLinkAggregateStale.class,
                store.prepareAggregate(
                        new ZLinkAggregatePrepareRequest(
                                aggregateId,
                                2,
                                participants,
                                new byte[32],
                                new ZLinkMeshNodeDescriptorKey(
                                        "game",
                                        targetRid),
                                22,
                                ZLinkPlacementCapacityBundle.actor(
                                        authorityKeys.size()),
                                target),
                        () -> false)
                    .toCompletableFuture().get());
            String targetLeaseKey = new ZLinkRedisLocationKeys(prefix)
                .leaseKey(target.ownerId());
            RedisClient leaseClient = RedisClient.create(options.redisUri());
            CompletableFuture<Void> restoreLease;
            try (var leaseConnection = leaseClient.connect()) {
                leaseConnection.sync().hset(
                    targetLeaseKey,
                    "expiresAt",
                    Long.toString(System.currentTimeMillis() - 1));
                restoreLease = CompletableFuture.runAsync(
                    () -> {
                        RedisClient restoreClient =
                            RedisClient.create(options.redisUri());
                        try (var restoreConnection = restoreClient.connect()) {
                            restoreConnection.sync().hset(
                                targetLeaseKey,
                                "expiresAt",
                                Long.toString(
                                    System.currentTimeMillis() + 30_000));
                        } finally {
                            restoreClient.shutdown();
                        }
                    },
                    CompletableFuture.delayedExecutor(
                        100,
                        TimeUnit.MILLISECONDS));
                try {
                    assertEquals(
                        ZLinkAggregateCommitResult.COMMITTED,
                        store.commitAggregate(
                                prepared.fence(),
                                () -> false)
                            .toCompletableFuture().get());
                } finally {
                    restoreLease.join();
                }
            } finally {
                leaseClient.shutdown();
            }
            assertEquals(
                ZLinkAggregateCommitResult.ALREADY_COMMITTED,
                store.commitAggregate(
                        prepared.fence(),
                        () -> false)
                    .toCompletableFuture().get());
            List<ZLinkAuthoritySnapshot> movedSnapshots =
                new ArrayList<>();
            for (String authorityKey : authorityKeys) {
                var moved = assertInstanceOf(
                    ZLinkAuthoritySnapshot.class,
                    store.read(authorityKey, () -> false)
                        .toCompletableFuture().get());
                assertEquals(target.ownerId(), moved.ownerId());
                movedSnapshots.add(moved);
            }
            assertArrayEquals(
                new byte[] {5},
                store.readAuthorityMembershipMutation(
                        authorityKeys.getFirst())
                    .toCompletableFuture().get());
            assertArrayEquals(
                new byte[] {6},
                store.readAuthorityMembershipMutation(
                        authorityKeys.get(1))
                    .toCompletableFuture().get());

            var reverseParticipants =
                IntStream.range(
                        0,
                        authorityKeys.size())
                    .mapToObj(index ->
                        new ZLinkAggregateParticipant(
                                authorityKeys.get(index),
                                movedSnapshots.get(index).objectGeneration(),
                                movedSnapshots.get(index)
                                    .authorityOwnerGeneration(),
                                movedSnapshots.get(index)
                                    .storeVersion(),
                                ZLinkAuthorityGenerationTransition
                                    .NEW_OWNER,
                                new byte[] {(byte) (7 + index)},
                                new byte[] {(byte) (9 + index)}))
                    .toList();
            var reversePrepared = assertInstanceOf(
                ZLinkAggregatePrepared.class,
                store.prepareAggregate(
                        new ZLinkAggregatePrepareRequest(
                                UUID.randomUUID(),
                                1,
                                reverseParticipants,
                                new byte[32],
                                new ZLinkMeshNodeDescriptorKey(
                                        "game",
                                        sourceRid),
                                21,
                                ZLinkPlacementCapacityBundle.actor(
                                        authorityKeys.size()),
                                source),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkAggregateAbortResult.ABORTED,
                store.abortAggregate(
                        reversePrepared.fence(),
                        () -> false)
                    .toCompletableFuture().get());
            assertEquals(
                ZLinkAggregateAbortResult.ALREADY_ABORTED,
                store.abortAggregate(
                        reversePrepared.fence(),
                        () -> false)
                    .toCompletableFuture().get());
            for (String authorityKey : authorityKeys) {
                assertEquals(
                    target.ownerId(),
                    assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        store.read(authorityKey, () -> false)
                            .toCompletableFuture().get())
                        .ownerId());
            }
        }
    }

    private static JsonNode authorityFixture() throws Exception {
        return redisFixture("authority-store-v1.json");
    }

    private static JsonNode descriptorFixture() throws Exception {
        return redisFixture("mesh-node-descriptor-v1.json");
    }

    private static JsonNode redisFixture(String fileName)
        throws Exception {
        Path current = Path.of("").toAbsolutePath();
        while (current != null) {
            for (Path candidate : List.of(
                current.resolve(
                    "framework/testdata/location/redis/"
                        + fileName),
                current.resolve(
                    "testdata/location/redis/"
                        + fileName))) {
                if (Files.isRegularFile(candidate)) {
                    return JSON.readTree(candidate.toFile());
                }
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            fileName + " was not found");
    }

    private static String capacityTypeBucket(
        String descriptor,
        String lifecycle,
        String kind,
        String stableType) {
        return capacitySegment(descriptor)
            + capacitySegment(lifecycle)
            + capacitySegment(kind)
            + capacitySegment(stableType);
    }

    private static String capacitySegment(String value) {
        return value.getBytes(StandardCharsets.UTF_8).length
            + ":"
            + value;
    }

    private static ZLinkObjectReservationRequest reservationRequest(
            String authorityKey,
            ZLinkMeshNodeDescriptor
                descriptor,
            ZLinkLocationOwnerToken owner,
            String stableType) {
        return new ZLinkObjectReservationRequest(
                ZLinkPlacementObjectKind.ACTOR,
                authorityKey,
                stableType,
                "creation-root",
                new byte[32],
                32,
                new ZLinkMeshNodeDescriptorKey(
                        descriptor.meshName(),
                        descriptor.rid()),
                descriptor.lifecycleGeneration(),
                owner,
                new byte[] {1},
                ZLinkPlacementCapacityBundle.actor(1));
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid,
            long lifecycleGeneration,
            long descriptorRevision,
            ZLinkLocationOwnerToken owner,
            String stableType,
            int activeLimit,
            int pendingLimit) {
        return descriptor(
            rid,
            lifecycleGeneration,
            descriptorRevision,
            owner,
            stableType,
            activeLimit,
            pendingLimit,
            "entry-" + rid);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            RoutingId rid,
            long lifecycleGeneration,
            long descriptorRevision,
            ZLinkLocationOwnerToken owner,
            String stableType,
            int activeLimit,
            int pendingLimit,
            String entrySpotId) {
        return new ZLinkMeshNodeDescriptor(
                "game",
                rid,
                lifecycleGeneration,
                descriptorRevision,
                "tcp://127.0.0.1:7000",
                Map.of("game", 100),
                1,
                List.of(
                    new ZLinkObjectCapability(
                            ZLinkPlacementObjectKind.ACTOR,
                            stableType,
                            ZLinkObjectMaintenancePolicyKind
                                .SNAPSHOT,
                            true,
                            0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of(entrySpotId),
                100,
                actorCapacity(0, 0, activeLimit),
                new ZLinkActivationConcurrency(0, 128),
                Optional.empty(),
                systems.zlink.framework.runtime.host
                    .ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                UPDATED_AT);
    }

    private static ZLinkPlacementCapacity actorCapacity(
            int active,
            int reserved,
            int limit) {
        return new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(active, reserved, limit),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of());
    }

    private static ZLinkPlacementCapacity spotCapacity(
            int active,
            int reserved,
            int populationLimit,
            int typeActive,
            int typeReserved,
            int typeLimit) {
        return new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(
                        active, reserved, populationLimit),
                List.of(new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "lobby",
                        new ZLinkCapacityUsage(
                                typeActive,
                                typeReserved,
                                typeLimit))));
    }
}
