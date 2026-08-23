package systems.zlink.framework.runtime.internal.locations;
import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.stream.IntStream;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Constructor;
import java.lang.reflect.Method;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.regex.Pattern;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;

final class ZLinkProviderAuthorityRepositoryTest {
    @Test
    void opaqueProviderCapacityIsReservedCommittedAndReleasedAtomically()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner = ((ZLinkOwnerLeaseClaimed) owners.claim(
                "owner-capacity", Duration.ofMinutes(1))
            .toCompletableFuture().get()).token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        var repository = new ZLinkProviderAuthorityRepository(
            provider, descriptors);
        var first = capacityRequest(
            ZLinkAuthorityKeyCodec.spot("spot-a"), descriptor, owner);
        var second = capacityRequest(
            ZLinkAuthorityKeyCodec.spot("spot-b"), descriptor, owner);

        var reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            repository.reserve(first, () -> false)
                .toCompletableFuture().get()).reservation();
        assertCounterValue(provider, "zlink:v11:object-counter", "2");
        assertCounterValue(
            provider, "zlink:v11:authority-owner-counter", "2");
        assertInstanceOf(
            ZLinkPlacementCapacityExhausted.class,
            repository.reserve(second, () -> false)
                .toCompletableFuture().get());
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            repository.commit(reservation, new byte[] {2}, null, () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkPlacementCapacityExhausted.class,
            repository.reserve(second, () -> false)
                .toCompletableFuture().get());

        var current = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            repository.read(
                    ZLinkAuthorityKeyCodec.spot("spot-a"), () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkAuthorityDeleted.class,
            repository.compareExchange(
                    ZLinkAuthorityKeyCodec.spot("spot-a"),
                    new ZLinkAuthorityExpectFound(current.storeVersion()),
                    new ZLinkAuthorityDelete(),
                    () -> false)
                .toCompletableFuture().get());
        assertInstanceOf(
            ZLinkObjectReserved.class,
            repository.reserve(second, () -> false)
                .toCompletableFuture().get());
    }

    @Test
    void capacityRowsUseTheNodeCppCanonicalKeyAndJsonShape()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owner = ((ZLinkOwnerLeaseClaimed)
            new ZLinkProviderOwnerLeaseRepository(provider).claim(
                "owner-canonical-capacity", Duration.ofMinutes(1))
            .toCompletableFuture().get()).token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(
            owner, "mesh /한", RoutingId.from("node /#?"));
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());

        var repository = new ZLinkProviderAuthorityRepository(
            provider, descriptors);
        var reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            repository.reserve(
                    capacityRequest(
                        ZLinkAuthorityKeyCodec.spot("spot-canonical"),
                        descriptor,
                        owner),
                    () -> false)
                .toCompletableFuture().get()).reservation();
        String key = "zlink:v11:capacity:mesh%20%2F%ED%95%9C:node%20%2F%23%3F";
        assertCapacityRow(
            provider,
            key,
            "{\"active\":{\"actors\":0,\"spots\":0,\"spotTypes\":{}},"
                + "\"pending\":{\"actors\":0,\"spots\":1,\"spotTypes\":{"
                + "\"user_spot\\u0000room\":1}}}");

        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            repository.commit(reservation, new byte[] {2}, null, () -> false)
                .toCompletableFuture().get());
        assertCapacityRow(
            provider,
            key,
            "{\"active\":{\"actors\":0,\"spots\":1,\"spotTypes\":{"
                + "\"user_spot\\u0000room\":1}},\"pending\":{\"actors\":0,"
                + "\"spots\":0,\"spotTypes\":{}}}");
    }

    @Test
    void exhaustedObjectCounterReturnsTypedResultWithoutChangingIt()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner = ((ZLinkOwnerLeaseClaimed) owners.claim(
                "owner-exhausted", Duration.ofMinutes(1))
            .toCompletableFuture().get()).token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        assertEquals(
            ZLinkLocationWriteStatus.STORED,
            descriptors.updateMeshNode(
                    descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get().status());
        ZLinkStoreKey counterKey = new ZLinkStoreKey(
            "zlink:v11:object-counter");
        provider.write(
                new ZLinkStoreWriteRequest(
                    List.of(),
                    List.of(new ZLinkStorePut(
                        counterKey,
                        Long.toString(Long.MAX_VALUE).getBytes(
                            java.nio.charset.StandardCharsets.UTF_8),
                        null))),
                () -> false)
            .toCompletableFuture().get();

        var result = new ZLinkProviderAuthorityRepository(
                provider, descriptors)
            .reserve(
                capacityRequest(
                    ZLinkAuthorityKeyCodec.spot("spot-exhausted"),
                    descriptor,
                    owner),
                () -> false)
            .toCompletableFuture().get();

        assertInstanceOf(ZLinkObjectGenerationExhausted.class, result);
        var counter = assertInstanceOf(
            ZLinkStoreReadFound.class,
            provider.read(counterKey, () -> false).toCompletableFuture().get());
        assertEquals(
            Long.toString(Long.MAX_VALUE),
            new String(
                counter.value().bytes(),
                java.nio.charset.StandardCharsets.UTF_8));
    }

    @Test
    void reserveRejectsNonCanonicalObjectAndOwnerCounterRecords()
        throws Exception {
        for (String counterName : List.of(
                "zlink:v11:object-counter",
                "zlink:v11:authority-owner-counter")) {
            for (String invalid : List.of("01", "+1", "0", "", " 1")) {
                var provider = new ZLinkInMemoryProviderLocationStore();
                var owner = ((ZLinkOwnerLeaseClaimed)
                    new ZLinkProviderOwnerLeaseRepository(provider)
                        .claim("owner-counter", Duration.ofMinutes(1))
                        .toCompletableFuture().get()).token();
                var descriptors = new ZLinkProviderDescriptorRepository(provider);
                var descriptor = capacityDescriptor(owner);
                assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    descriptors.updateMeshNode(
                            descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture().get().status());
                provider.write(
                        new ZLinkStoreWriteRequest(
                            List.of(),
                            List.of(new ZLinkStorePut(
                                new ZLinkStoreKey(counterName),
                                invalid.getBytes(
                                    java.nio.charset.StandardCharsets.UTF_8),
                                null))),
                        () -> false)
                    .toCompletableFuture().get();

                var failure = assertThrows(
                    java.util.concurrent.ExecutionException.class,
                    () -> new ZLinkProviderAuthorityRepository(provider, descriptors)
                        .reserve(
                            capacityRequest(
                                ZLinkAuthorityKeyCodec.spot("spot-counter"),
                                descriptor,
                                owner),
                            () -> false)
                        .toCompletableFuture().get());
                assertInstanceOf(IllegalStateException.class, failure.getCause());
                assertEquals(
                    "Location Store counter is invalid",
                    failure.getCause().getMessage());
            }
        }
    }

    @Test
    void nearCeilingAggregateCounterBlockReturnsTypedExhaustion()
        throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owner = ((ZLinkOwnerLeaseClaimed)
            new ZLinkProviderOwnerLeaseRepository(provider)
                .claim("owner-a", Duration.ofMinutes(1))
                .toCompletableFuture().get()).token();
        String authorityA = ZLinkAuthorityKeyCodec.actor("ceiling-a");
        String authorityB = ZLinkAuthorityKeyCodec.actor("ceiling-b");
        var keyA = authorityKey(authorityA);
        var keyB = authorityKey(authorityB);
        var seeded = (systems.zlink.framework.locationprovider
                .ZLinkStoreWriteApplied) provider.write(
                    new ZLinkStoreWriteRequest(
                        List.of(),
                        List.of(
                            new ZLinkStorePut(keyA, encodedAuthorityRecord(), null),
                            new ZLinkStorePut(keyB, encodedAuthorityRecord(), null),
                            new ZLinkStorePut(
                                new ZLinkStoreKey(
                                    "zlink:v11:authority-owner-counter"),
                                Long.toString(Long.MAX_VALUE - 1).getBytes(
                                    java.nio.charset.StandardCharsets.UTF_8),
                                null))),
                    () -> false).toCompletableFuture().get();
        var request = new ZLinkAggregatePrepareRequest(
            new UUID(0, 12),
            1,
            List.of(
                new ZLinkAggregateParticipant(
                    authorityA, 1, 1, seeded.putVersions().get(keyA).value(),
                    ZLinkAuthorityGenerationTransition.NEW_OWNER,
                    new byte[] {1}, new byte[] {2}),
                new ZLinkAggregateParticipant(
                    authorityB, 1, 1, seeded.putVersions().get(keyB).value(),
                    ZLinkAuthorityGenerationTransition.NEW_OWNER,
                    new byte[] {3}, new byte[] {4})),
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
            1,
            ZLinkPlacementCapacityBundle.actor(1),
            owner);

        var result = new ZLinkProviderAuthorityRepository(provider)
            .prepareAggregate(request, () -> false).toCompletableFuture().get();

        assertInstanceOf(ZLinkAggregateGenerationExhausted.class, result);
        assertCounterValue(
            provider,
            "zlink:v11:authority-owner-counter",
            Long.toString(Long.MAX_VALUE - 1));
    }

    @Test
    void aggregateMarkerRoundTripPreservesCompletionCounters()
        throws ReflectiveOperationException {
        var request = new ZLinkAggregatePrepareRequest(
            new UUID(0, 9),
            7,
            List.of(
                participant("actor:a"),
                participant("spot:b")),
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            4,
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken("owner-b", 12));
        Method encode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod(
                "encodeAggregate",
                byte.class,
                ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        byte[] encoded = (byte[]) encode.invoke(null, (byte) 2, request);

        Method decode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod("decodeAggregate", byte[].class);
        decode.setAccessible(true);
        Object decoded = decode.invoke(null, encoded);
        Method decodedId = decoded.getClass().getDeclaredMethod("aggregateId");
        decodedId.setAccessible(true);

        assertEquals(request.aggregateId(), decodedId.invoke(decoded));
    }

    @Test
    void stagingMarkerStoresMetadataInsteadOfParticipantPayloads()
        throws ReflectiveOperationException {
        var participants = IntStream.range(0, 2050)
            .mapToObj(index -> participant("authority-%04d".formatted(index)))
            .toList();
        var request = new ZLinkAggregatePrepareRequest(
            new UUID(0, 10),
            8,
            participants,
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
            4,
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken("owner-b", 12));

        Method encode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod(
                "encodeAggregate",
                byte.class,
                ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        byte[] encoded = (byte[]) encode.invoke(
            null,
            (byte) 0,
            request);

        assertTrue(encoded.length < 1024);

        Method decode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod("decodeAggregate", byte[].class);
        decode.setAccessible(true);
        Object decoded = decode.invoke(null, encoded);
        Method state = decoded.getClass().getDeclaredMethod("state");
        Method participantCount = decoded.getClass()
            .getDeclaredMethod("participantCount");
        state.setAccessible(true);
        participantCount.setAccessible(true);

        assertEquals((byte) 0, state.invoke(decoded));
        assertEquals(2050, participantCount.invoke(decoded));
    }

    @Test
    void ownerCleanupContinuesAfterTheFirstProviderScanPage()
        throws ReflectiveOperationException {
        var store = new ZLinkInMemoryProviderLocationStore();
        var encodedAuthority = encodedAuthorityRecord();
        var mutations = new ArrayList<systems.zlink.framework.locationprovider
            .ZLinkStoreMutation>();
        for (int index = 0; index < 1_001; index++) {
            String authorityKey = "authority\0actor\0authority-" + index;
            mutations.add(new ZLinkStorePut(
                new ZLinkStoreKey(authorityKey),
                encodedAuthority,
                null));
        }
        store.write(
                new ZLinkStoreWriteRequest(List.of(), mutations),
                () -> false)
            .toCompletableFuture()
            .join();

        var repository = new ZLinkProviderAuthorityRepository(store);
        long removed = repository.removeAllByOwner(
                new ZLinkLocationOwnerToken("owner-a", 1))
            .toCompletableFuture()
            .join();

        assertEquals(1_001L, removed);
        var remaining = (ZLinkStoreScanPageResult) store.scan(
                new ZLinkStoreScanRequest(
                    "authority\0", null, 1_000),
                () -> false)
            .toCompletableFuture()
            .join();
        assertTrue(remaining.value().items().isEmpty());
    }

    @Test
    void rollbackDoesNotRemoveAnExistingParticipantMarker()
        throws ReflectiveOperationException {
        String authorityAContractKey = ZLinkAuthorityKeyCodec.actor(
            "authority-a");
        String authorityBContractKey = ZLinkAuthorityKeyCodec.actor(
            "authority-b");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var store = new FailingParticipantStore(
            delegate, authorityBContractKey);
        var ownerLeases = new ZLinkProviderOwnerLeaseRepository(store);
        var owner = (ZLinkOwnerLeaseClaimed) ownerLeases.claim(
                "owner-a", Duration.ofHours(1))
            .toCompletableFuture()
            .join();
        var authorityA = authorityKey(authorityAContractKey);
        var authorityB = authorityKey(authorityBContractKey);
        var seed = encodedAuthorityRecord();
        var seeded = (systems.zlink.framework.locationprovider
                .ZLinkStoreWriteApplied) delegate.write(
                    new ZLinkStoreWriteRequest(
                        List.of(),
                        List.of(
                            new ZLinkStorePut(authorityA, seed, null),
                            new ZLinkStorePut(authorityB, seed, null))),
                    () -> false)
            .toCompletableFuture()
            .join();
        String versionA = seeded.putVersions().get(authorityA).value();
        String versionB = seeded.putVersions().get(authorityB).value();
        var participantA = participant(
            authorityAContractKey, versionA, new byte[] {11}, new byte[] {12});
        var participantB = participant(
            authorityBContractKey, versionB, new byte[] {21}, new byte[] {22});
        var request = new ZLinkAggregatePrepareRequest(
            new UUID(0, 11),
            1,
            List.of(participantA, participantB),
            new byte[32],
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
            1,
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken(
                owner.token().ownerId(), owner.token().leaseGeneration()));

        Object marker = aggregateParticipantMarker(request, participantA, 0);
        delegate.write(
                new ZLinkStoreWriteRequest(
                    List.of(),
                    List.of(new ZLinkStorePut(
                        authorityA,
                        encodedAuthorityRecord(marker),
                        null))),
                () -> false)
            .toCompletableFuture()
            .join();
        delegate.write(
                new ZLinkStoreWriteRequest(
                    List.of(),
                    List.of(new ZLinkStorePut(
                        aggregateKey(request),
                        encodedAggregateStaging(request),
                        null))),
                () -> false)
            .toCompletableFuture()
            .join();

        var repository = new ZLinkProviderAuthorityRepository(store);
        var result = repository.prepareAggregate(request, () -> false)
            .toCompletableFuture()
            .join();

        assertTrue(
            store.failingWrites > 0,
            () -> "participant write was not intercepted; result=" + result);
        assertEquals(
            ZLinkAggregateConflict.class,
            result.getClass(),
            () -> "unexpected prepare result: " + result);
        var decoded = decodeAuthority(
            ((ZLinkStoreReadFound)
                delegate.read(authorityA, () -> false)
                    .toCompletableFuture()
                    .join()).value().bytes());
        Method aggregate = decoded.getClass().getDeclaredMethod("aggregate");
        aggregate.setAccessible(true);
        assertNotNull(aggregate.invoke(decoded));
    }

    @Test
    void aggregateCommitRetriesAConditionalWriteConflict()
        throws ReflectiveOperationException {
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var store = new FailingParticipantStore(delegate, null, true);
        var owner = (ZLinkOwnerLeaseClaimed) new ZLinkProviderOwnerLeaseRepository(
                store)
            .claim("owner-a", Duration.ofHours(1))
            .toCompletableFuture()
            .join();
        String authorityAContractKey = ZLinkAuthorityKeyCodec.actor(
            "authority-a");
        var authorityKey = authorityKey(authorityAContractKey);
        var basePayload = new ZLinkActorAuthorityPayloadCodec().encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "Actor",
            "actor-a",
            "spot-a",
            1,
            1,
            owner.token().ownerId(),
            owner.token().leaseGeneration(),
            "game",
            RoutingId.from("node-a"),
            1);
        var seed = (systems.zlink.framework.locationprovider
                .ZLinkStoreWriteApplied) delegate.write(
                    new ZLinkStoreWriteRequest(
                        List.of(),
                        List.of(new ZLinkStorePut(
                            authorityKey,
                            encodedAuthorityRecord(null, basePayload),
                            null))),
                    () -> false)
            .toCompletableFuture()
            .join();
        String version = seed.putVersions().get(authorityKey).value();
        var relocationRequest = new ZLinkAggregateRelocationCoordinator.Request(
            new UUID(0, 9),
            1,
            2,
            List.of(new ZLinkAggregateRelocationCoordinator.Participant(
                authorityAContractKey,
                ZLinkPlacementObjectKind.ACTOR,
                1,
                1,
                version,
                ZLinkAuthorityGenerationTransition.PRESERVE,
                basePayload,
                new byte[0])),
            goldenRoot(),
            new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
            1,
            ZLinkPlacementCapacityBundle.actor(1),
            owner.token(),
            version);
        byte[] canonicalPayload =
            ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                basePayload,
                relocationRequest,
                ZLinkAuthorityGenerationTransition.PRESERVE);
        var participant = new ZLinkAggregateParticipant(
            authorityAContractKey,
            1,
            1,
            version,
            ZLinkAuthorityGenerationTransition.PRESERVE,
            canonicalPayload,
            new byte[0]);
        var request = new ZLinkAggregatePrepareRequest(
            relocationRequest.aggregateId(),
            relocationRequest.aggregateGeneration(),
            List.of(participant),
            new byte[32],
            relocationRequest.targetDescriptor(),
            relocationRequest.targetDescriptorLifecycleGeneration(),
            relocationRequest.capacityBundle(),
            relocationRequest.targetOwner());
        Object marker = aggregateParticipantMarker(request, participant, 0);
        delegate.write(
                new ZLinkStoreWriteRequest(
                    List.of(),
                    List.of(new ZLinkStorePut(
                        authorityKey,
                        encodedAuthorityRecord(marker, basePayload),
                        null))),
                () -> false)
            .toCompletableFuture()
            .join();
        new ZLinkAggregateInventoryStore(store).store(request, () -> false)
            .toCompletableFuture()
            .join();
        delegate.write(
                new ZLinkStoreWriteRequest(
                    List.of(),
                    List.of(new ZLinkStorePut(
                        aggregateKey(request),
                        encodedAggregate((byte) 1, request),
                        null))),
                () -> false)
            .toCompletableFuture()
            .join();

        var result = new ZLinkProviderAuthorityRepository(store)
            .commitAggregate(
                new ZLinkAggregateFence(
                    request.aggregateId(), request.aggregateGeneration()),
                () -> false)
            .toCompletableFuture()
            .join();

        assertEquals(ZLinkAggregateCommitResult.COMMITTED, result);
        var committed = (systems.zlink.framework.locationprovider
                .ZLinkStoreReadFound) delegate.read(
                    aggregateKey(request), () -> false)
            .toCompletableFuture()
            .join();
        Method state = decodeAggregate(committed.value().bytes())
            .getClass()
            .getDeclaredMethod("state");
        state.setAccessible(true);
        assertEquals((byte) 2, state.invoke(
            decodeAggregate(committed.value().bytes())));
    }

    private static byte[] encodedAuthorityRecord()
        throws ReflectiveOperationException {
        return encodedAuthorityRecord(null);
    }

    private static ZLinkObjectReservationRequest capacityRequest(
        String authorityKey,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner) {
        return new ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind.USER_SPOT,
            authorityKey,
            "room",
            "inline-v1:test",
            new byte[32],
            4,
            new ZLinkMeshNodeDescriptorKey(
                descriptor.meshName(), descriptor.rid()),
            descriptor.lifecycleGeneration(),
            owner,
            new byte[] {1},
            ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT,
                "room",
                1));
    }

    private static void assertCounterValue(
        ZLinkLocationStore provider,
        String key,
        String expected) throws Exception {
        var found = assertInstanceOf(
            ZLinkStoreReadFound.class,
            provider.read(new ZLinkStoreKey(key), () -> false)
                .toCompletableFuture().get());
        assertEquals(
            expected,
            new String(
                found.value().bytes(),
                java.nio.charset.StandardCharsets.UTF_8));
    }

    private static void assertCapacityRow(
        ZLinkLocationStore provider,
        String key,
        String expected) throws Exception {
        var found = assertInstanceOf(
            ZLinkStoreReadFound.class,
            provider.read(new ZLinkStoreKey(key), () -> false)
                .toCompletableFuture().get());
        assertEquals(
            expected,
            new String(
                found.value().bytes(),
                java.nio.charset.StandardCharsets.UTF_8));
    }

    private static ZLinkMeshNodeDescriptor capacityDescriptor(
        ZLinkLocationOwnerToken owner) {
        return capacityDescriptor(owner, "game", RoutingId.from("capacity-node"));
    }

    private static ZLinkMeshNodeDescriptor capacityDescriptor(
        ZLinkLocationOwnerToken owner,
        String meshName,
        RoutingId nodeRid) {
        return new ZLinkMeshNodeDescriptor(
            meshName,
            nodeRid,
            1,
            1,
            "tcp://127.0.0.1:7100",
            Map.of(),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                "room",
                ZLinkObjectMaintenancePolicyKind.DISABLED,
                false,
                1)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(
                "capacity-node-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 1),
                new ZLinkCapacityUsage(0, 0, 1),
                List.of()),
            new ZLinkActivationConcurrency(0, 64),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "capacity-security",
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.parse("2026-08-06T00:00:00Z"));
    }

    private static byte[] encodedAuthorityRecord(Object aggregate)
        throws ReflectiveOperationException {
        return encodedAuthorityRecord(aggregate, new byte[] {1, 2, 3});
    }

    private static byte[] encodedAuthorityRecord(
        Object aggregate,
        byte[] payload)
        throws ReflectiveOperationException {
        Class<?> authorityRecord = Arrays.stream(
                ZLinkProviderAuthorityRepository.class.getDeclaredClasses())
            .filter(type -> type.getSimpleName().equals("AuthorityRecord"))
            .findFirst()
            .orElseThrow();
        Constructor<?> constructor = Arrays.stream(
                authorityRecord.getDeclaredConstructors())
            .filter(value -> value.getParameterCount() == 9)
            .findFirst()
            .orElseThrow();
        constructor.setAccessible(true);
        var allocation = new ZLinkPlacementAllocation(
            ZLinkPlacementAllocationState.ACTIVE,
            ZLinkPlacementObjectKind.ACTOR,
            "actor",
            new ZLinkMeshNodeDescriptorKey(
                "game", RoutingId.from("node-a")),
            1,
            ZLinkPlacementCapacityBundle.actor(1));
        Object record = constructor.newInstance(
            payload,
            1L,
            1L,
            "owner-a",
            1L,
            allocation,
            Optional.empty(),
            aggregate,
            null);
        Method encode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod("encode", authorityRecord);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(null, record);
    }

    private static ZLinkAggregateParticipant participant(String key) {
        return participant(key, "version-1", new byte[] {1}, new byte[] {2});
    }

    private static ZLinkAggregateParticipant participant(
        String key,
        String version,
        byte[] authorityPayload,
        byte[] membershipMutation) {
        return new ZLinkAggregateParticipant(
            key,
            3,
            5,
            version,
            ZLinkAuthorityGenerationTransition.PRESERVE,
            authorityPayload,
            membershipMutation);
    }

    private static Object aggregateParticipantMarker(
        ZLinkAggregatePrepareRequest request,
        ZLinkAggregateParticipant participant,
        int index)
        throws ReflectiveOperationException {
        Class<?> marker = Arrays.stream(
                ZLinkProviderAuthorityRepository.class.getDeclaredClasses())
            .filter(type -> type.getSimpleName()
                .equals("AggregateParticipantMarker"))
            .findFirst()
            .orElseThrow();
        Constructor<?> constructor = marker.getDeclaredConstructors()[0];
        constructor.setAccessible(true);
        Method sha256 = ZLinkAggregateInventoryStore.class.getDeclaredMethod(
            "sha256", byte[].class);
        sha256.setAccessible(true);
        return constructor.newInstance(
            request.aggregateId(),
            request.aggregateGeneration(),
            index,
            participant.expectedStoreVersion(),
            participant.ownerTransition(),
            1L,
            sha256.invoke(null, participant.authorityPayload()),
            sha256.invoke(null, participant.membershipMutation()));
    }

    private static byte[] encodedAggregateStaging(
        ZLinkAggregatePrepareRequest request)
        throws ReflectiveOperationException {
        return encodedAggregate((byte) 0, request);
    }

    private static byte[] encodedAggregate(
        byte state,
        ZLinkAggregatePrepareRequest request)
        throws ReflectiveOperationException {
        Method encode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod(
                "encodeAggregate",
                byte.class,
                ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(null, state, request);
    }

    private static byte[] encodedCommittedAggregate(
        ZLinkAggregatePrepareRequest request)
        throws ReflectiveOperationException {
        Method encode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod(
                "encodeAggregate",
                byte.class,
                ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(
            null,
            (byte) 2,
            request);
    }

    private static Object decodeAuthority(byte[] bytes)
        throws ReflectiveOperationException {
        Method decode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod("decode", byte[].class);
        decode.setAccessible(true);
        return decode.invoke(null, bytes);
    }

    private static Object decodeAggregate(byte[] bytes)
        throws ReflectiveOperationException {
        Method decode = ZLinkProviderAuthorityRepository.class
            .getDeclaredMethod("decodeAggregate", byte[].class);
        decode.setAccessible(true);
        return decode.invoke(null, bytes);
    }

    private static byte stateOf(byte[] bytes)
        throws ReflectiveOperationException {
        Object aggregate = decodeAggregate(bytes);
        Method state = aggregate.getClass().getDeclaredMethod("state");
        state.setAccessible(true);
        return (byte) state.invoke(aggregate);
    }

    // Mirrors the production ZLinkProviderAuthorityRepository.authorityKey()
    // canonical preimage (21-location-runtime.md#2.4) so direct-seeded rows
    // land where the aggregate/reserve/commit code paths actually look.
    private static ZLinkStoreKey authorityKey(String key) {
        var identity = ZLinkAuthorityKeyCodec.decode(key);
        return new ZLinkStoreKey(
            "authority\0" + identity.kind() + "\0" + identity.id());
    }

    private static ZLinkStoreKey aggregateKey(
        ZLinkAggregatePrepareRequest request) {
        return new ZLinkStoreKey(
            "zlink:v11:aggregate:" + request.aggregateId()
                + ":" + request.aggregateGeneration());
    }

    private static byte[] goldenRoot() {
        Path current = Path.of(System.getProperty("user.dir"))
            .toAbsolutePath();
        Pattern logicalHex = Pattern.compile(
            "\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"");
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(fixture)) {
                try {
                    var match = logicalHex.matcher(Files.readString(fixture));
                    if (match.find()) {
                        return HexFormat.of().parseHex(match.group(1));
                    }
                } catch (IOException failure) {
                    throw new IllegalStateException(failure);
                }
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found");
    }

    private static final class FailingParticipantStore
        implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final ZLinkStoreKey failingKey;
        private boolean failCommitOnce;
        private int failingWrites;

        private FailingParticipantStore(
            ZLinkLocationStore delegate,
            String failingAuthority) {
            this(delegate, failingAuthority, false);
        }

        private FailingParticipantStore(
            ZLinkLocationStore delegate,
            String failingAuthority,
            boolean failCommitOnce) {
            this.delegate = delegate;
            this.failingKey = failingAuthority == null
                ? null
                : authorityKey(failingAuthority);
            this.failCommitOnce = failCommitOnce;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
            ZLinkStoreKey key,
            ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
            ZLinkStoreWriteRequest request,
            ZLinkStoreCancellation cancellation) {
            boolean writesFailingParticipant = failingKey != null
                && request.mutations().stream()
                    .anyMatch(mutation -> mutation instanceof ZLinkStorePut put
                        && put.key().equals(failingKey));
            boolean writesCommittedAggregate = failCommitOnce
                && request.mutations().stream()
                    .anyMatch(mutation -> mutation instanceof ZLinkStorePut put
                        && put.bytes().length > 0
                        && put.key().value().startsWith("zlink:v11:aggregate:")
                        && put.bytes()[0] == 2);
            if (writesFailingParticipant || writesCommittedAggregate) {
                if (writesFailingParticipant) {
                    failingWrites++;
                }
                failCommitOnce = false;
                return CompletableFuture.completedFuture(
                    new systems.zlink.framework.locationprovider
                        .ZLinkStoreWriteConflict(Instant.now()));
            }
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
            ZLinkStoreScanRequest request,
            ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }
}
