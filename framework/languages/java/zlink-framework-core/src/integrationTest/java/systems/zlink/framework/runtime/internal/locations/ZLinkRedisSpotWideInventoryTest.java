package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationOptions;
import systems.zlink.framework.locations.redis.ZLinkRedisLocationStore;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkRequestFailureMapping;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicBoolean;

final class ZLinkRedisSpotWideInventoryTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;
    private static final String SPOT = ZLinkAuthorityKeyCodec.spot("matrix-room");
    private static final String ACTOR = ZLinkAuthorityKeyCodec.actor("matrix-actor");
    private static final byte[] SEALED_PAYLOAD = {7, 8};

    @ParameterizedTest(name = "changed member version: {0}")
    @ValueSource(booleans = {false, true})
    void readsSealedVersionsWithOnlyTheRepresentativeCaptured(boolean staleMember) {
        try (var provider =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix(
                                        "zlink:spotwide-inventory-test:" + UUID.randomUUID()))) {
            var repository = new ZLinkProviderLocationRepository(provider);
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease("source", Duration.ofMinutes(1))
                                            .toCompletableFuture()
                                            .join())
                            .token();
            var node = descriptor(owner);
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    repository
                            .updateMeshNode(node, ZLinkLocationWriteIntent.NEW_CLAIM)
                            .toCompletableFuture()
                            .join()
                            .status());
            var spot = activate(repository, node, owner, SPOT, ZLinkPlacementObjectKind.USER_SPOT);
            var actor = activate(repository, node, owner, ACTOR, ZLinkPlacementObjectKind.ACTOR);
            var participants =
                    List.of(
                            new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                    SPOT, spot.objectGeneration(), spot.authorityOwnerGeneration()),
                            new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                    ACTOR,
                                    actor.objectGeneration(),
                                    actor.authorityOwnerGeneration()));
            var coordinator = new ZLinkAggregateRelocationCoordinator(repository);
            var fence = ZLinkAggregateFence.forRelocation(UUID.randomUUID());
            var preparing =
                    coordinator
                            .prepareSource(
                                    participants, SPOT, fence, 1, ignored -> SEALED_PAYLOAD, OPEN)
                            .toCompletableFuture()
                            .join();
            var captured =
                    coordinator
                            .captureSource(participants, SPOT, fence, 1, OPEN)
                            .toCompletableFuture()
                            .join();
            assertEquals("captured", captured.relocation().phase());
            assertEquals(
                    preparing.relocation().sourceInventorySha256(),
                    captured.relocation().sourceInventorySha256());
            var reentered =
                    coordinator
                            .prepareSource(
                                    participants,
                                    SPOT,
                                    fence,
                                    1,
                                    ignored -> {
                                        throw new AssertionError("sealed participant was rebuilt");
                                    },
                                    OPEN)
                            .toCompletableFuture()
                            .join();
            assertEquals(captured.storeVersion(), reentered.storeVersion());
            assertNull(snapshot(repository, ACTOR).relocation());
            assertEquals(actor.storeVersion(), snapshot(repository, ACTOR).storeVersion());

            if (staleMember) {
                var updated =
                        assertInstanceOf(
                                ZLinkAuthorityStored.class,
                                repository
                                        .compareExchange(
                                                ACTOR,
                                                new ZLinkAuthorityExpectFound(actor.storeVersion()),
                                                new ZLinkAuthorityPut(actor.payload()),
                                                OPEN)
                                        .toCompletableFuture()
                                        .join());
                assertNotEquals(actor.storeVersion(), updated.storeVersion());
                var failure =
                        assertThrows(
                                CompletionException.class,
                                () ->
                                        coordinator
                                                .readSealedInventory(fence, SPOT, OPEN)
                                                .toCompletableFuture()
                                                .join());
                assertInstanceOf(
                        ZLinkAggregateRelocationCoordinator.RelocationDataLostException.class,
                        failure.getCause());
            } else {
                var restored =
                        coordinator
                                .readSealedInventory(fence, SPOT, OPEN)
                                .toCompletableFuture()
                                .join();
                assertEquals(
                        List.of(ACTOR, SPOT),
                        restored.stream().map(entry -> entry.authority().key()).toList());
                assertEquals(actor.storeVersion(), restored.getFirst().expectedStoreVersion());
                assertEquals(spot.storeVersion(), restored.getLast().expectedStoreVersion());
                assertArrayEquals(SEALED_PAYLOAD, restored.getFirst().authorityPayload());
                abortPreparedInventory(
                        repository,
                        coordinator,
                        fence,
                        participants.getFirst(),
                        captured,
                        restored,
                        node,
                        owner);
            }
        }
    }

    @ParameterizedTest(name = "replacement CAS response lost: {0}")
    @ValueSource(booleans = {false, true})
    void preparedReplacementWritesOnlyRepresentativeAndCommitsCurrentAttempt(boolean loseResponse)
            throws java.io.IOException {
        try (var provider =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix("zlink:spotwide-replacement:" + UUID.randomUUID()))) {
            var writes = new CopyOnWriteArrayList<ZLinkStoreWriteRequest>();
            var loseNextWriteResponse = new AtomicBoolean();
            var repository =
                    new ZLinkProviderLocationRepository(
                            recordingProvider(provider, writes, loseNextWriteResponse));
            var sourceOwner = claim(repository, "source");
            var targetOwner = claim(repository, "target");
            var source = descriptor(sourceOwner);
            var target = descriptor(targetOwner);
            for (var node : List.of(source, target)) {
                assertEquals(
                        ZLinkLocationWriteStatus.STORED,
                        repository
                                .updateMeshNode(node, ZLinkLocationWriteIntent.NEW_CLAIM)
                                .toCompletableFuture()
                                .join()
                                .status());
            }
            var spotCodec = new ZLinkServiceAuthorityPayloadCodec();
            var actorCodec = new ZLinkActorAuthorityPayloadCodec();
            byte[] sourceSpot =
                    spotCodec.encodeUser(
                            ZLinkServiceAuthorityPayloadCodec.State.READY,
                            "type",
                            "matrix-room",
                            sourceOwner.ownerId(),
                            sourceOwner.leaseGeneration(),
                            source.meshName(),
                            source.rid(),
                            source.lifecycleGeneration());
            var spot =
                    activate(
                            repository,
                            source,
                            sourceOwner,
                            SPOT,
                            ZLinkPlacementObjectKind.USER_SPOT,
                            sourceSpot);
            byte[] sourceActor =
                    actorCodec.encode(
                            ZLinkActorAuthorityPayloadCodec.State.READY,
                            "type",
                            "matrix-actor",
                            "matrix-room",
                            spot.objectGeneration(),
                            2,
                            sourceOwner.ownerId(),
                            sourceOwner.leaseGeneration(),
                            source.meshName(),
                            source.rid(),
                            source.lifecycleGeneration());
            var actor =
                    activate(
                            repository,
                            source,
                            sourceOwner,
                            ACTOR,
                            ZLinkPlacementObjectKind.ACTOR,
                            sourceActor);
            var expected =
                    List.of(
                            new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                    SPOT, spot.objectGeneration(), spot.authorityOwnerGeneration()),
                            new ZLinkAggregateRelocationCoordinator.ExpectedParticipant(
                                    ACTOR,
                                    actor.objectGeneration(),
                                    actor.authorityOwnerGeneration()));
            var coordinator = new ZLinkAggregateRelocationCoordinator(repository);
            var fence = ZLinkAggregateFence.forRelocation(UUID.randomUUID());
            coordinator
                    .prepareSource(expected, SPOT, fence, 1, ZLinkAuthoritySnapshot::payload, OPEN)
                    .toCompletableFuture()
                    .join();
            var captured =
                    coordinator
                            .captureSource(expected, SPOT, fence, 1, OPEN)
                            .toCompletableFuture()
                            .join();
            var descriptor = new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid());
            var aggregateKey = ZLinkProviderAuthorityRepository.aggregateKey(fence);
            var rootKey = ZLinkAggregateInventoryStore.rootKey(fence);
            var memberKey = ZLinkProviderAuthorityRepository.authorityKey(ACTOR);
            var representativeKey = ZLinkProviderAuthorityRepository.authorityKey(SPOT);
            var inventory =
                    coordinator.readSealedInventory(fence, SPOT, OPEN).toCompletableFuture().join();
            assertArrayEquals(sourceActor, inventory.getFirst().authorityPayload());
            assertArrayEquals(sourceSpot, inventory.getLast().authorityPayload());
            assertArrayEquals(new byte[] {'[', ']'}, inventory.getFirst().membershipMutation());
            var participants =
                    inventory.stream()
                            .map(
                                    item -> {
                                        var row = item.authority().snapshot();
                                        return new ZLinkAggregateRelocationCoordinator.Participant(
                                                item.authority().key(),
                                                row.allocation().objectKind(),
                                                row.objectGeneration(),
                                                row.authorityOwnerGeneration(),
                                                item.expectedStoreVersion(),
                                                item.ownerTransition(),
                                                item.authorityPayload(),
                                                item.membershipMutation(),
                                                row.storeVersion());
                                    })
                            .toList();
            byte[] envelope = spotEnvelope(fence, spot);
            var capacity =
                    new ZLinkPlacementCapacityBundle(
                            1,
                            1,
                            Optional.of(
                                    new ZLinkSpotTypeCapacityDelta(
                                            ZLinkPlacementObjectKind.USER_SPOT, "type", 1)));

            var first =
                    coordinator
                            .prepare(
                                    new ZLinkAggregateRelocationCoordinator.Request(
                                            fence.aggregateId(),
                                            1,
                                            1,
                                            participants,
                                            envelope,
                                            descriptor,
                                            target.lifecycleGeneration(),
                                            capacity,
                                            targetOwner,
                                            captured.relocation()
                                                    .coordinatorExpectedStoreVersion()),
                                    OPEN)
                            .toCompletableFuture()
                            .join();
            var aggregateBefore = read(provider, aggregateKey);
            var rootBefore = read(provider, rootKey);
            var memberBefore = read(provider, memberKey);
            var mainBefore = read(provider, representativeKey);
            writes.clear();
            loseNextWriteResponse.set(loseResponse);

            var second =
                    coordinator
                            .prepare(
                                    new ZLinkAggregateRelocationCoordinator.Request(
                                            fence.aggregateId(),
                                            1,
                                            2,
                                            participants,
                                            envelope,
                                            descriptor,
                                            target.lifecycleGeneration(),
                                            capacity,
                                            targetOwner,
                                            captured.relocation()
                                                    .coordinatorExpectedStoreVersion()),
                                    OPEN)
                            .toCompletableFuture()
                            .join();

            assertEquals(1, writes.size());
            assertEquals(
                    List.of(
                            new ZLinkStoreVersionCondition(
                                    representativeKey, mainBefore.version())),
                    writes.getFirst().conditions());
            assertEquals(1, writes.getFirst().mutations().size());
            assertEquals(representativeKey, writes.getFirst().mutations().getFirst().key());
            assertEquals(aggregateBefore.version(), read(provider, aggregateKey).version());
            assertEquals(rootBefore.version(), read(provider, rootKey).version());
            assertEquals(memberBefore.version(), read(provider, memberKey).version());
            assertEquals(
                    2,
                    snapshot(repository, SPOT)
                            .relocation()
                            .target()
                            .get("attemptGeneration")
                            .asLong());
            assertNull(snapshot(repository, ACTOR).relocation());

            var mismatchedTarget =
                    assertThrows(
                            CompletionException.class,
                            () ->
                                    coordinator
                                            .readSealedInventory(
                                                    fence,
                                                    SPOT,
                                                    source.rid(),
                                                    source.lifecycleGeneration(),
                                                    sourceOwner,
                                                    2,
                                                    OPEN)
                                            .toCompletableFuture()
                                            .join());
            assertInstanceOf(ZLinkFrameworkException.class, mismatchedTarget.getCause());
            assertEquals(
                    (int) ServiceWireConstants.FRAMEWORK_ERROR_ACTOR_LOCATION_STALE,
                    ZLinkRequestFailureMapping.causeCode(mismatchedTarget.getCause()));

            assertThrows(
                    CompletionException.class,
                    () -> coordinator.commit(first, OPEN).toCompletableFuture().join());
            assertEquals(sourceOwner.ownerId(), snapshot(repository, SPOT).ownerId());

            coordinator.commit(second, OPEN).toCompletableFuture().join();
            assertThrows(
                    CompletionException.class,
                    () -> coordinator.commit(first, OPEN).toCompletableFuture().join());
            var movedSpot = spotCodec.decode(snapshot(repository, SPOT).payload()).orElseThrow();
            var movedActor = actorCodec.decode(snapshot(repository, ACTOR).payload()).orElseThrow();
            assertEquals(targetOwner.ownerId(), movedSpot.ownerId());
            assertEquals(target.lifecycleGeneration(), movedSpot.nodeGeneration());
            assertEquals(targetOwner.ownerId(), movedActor.ownerId());
            assertEquals(target.rid(), movedActor.nodeRid());
            assertEquals("matrix-room", movedActor.currentSpotId());
            assertEquals(
                    2,
                    snapshot(repository, ACTOR)
                            .relocation()
                            .target()
                            .get("attemptGeneration")
                            .asLong());
        }
    }

    private static byte[] spotEnvelope(ZLinkAggregateFence fence, ZLinkAuthoritySnapshot spot)
            throws java.io.IOException {
        return ServiceWirePilotCodec.encodeRelocationEnvelopeV1(
                new ServiceWirePilotCodec.RelocationEnvelopeV1(
                        fence.aggregateId().getMostSignificantBits(),
                        fence.aggregateId().getLeastSignificantBits(),
                        new ServiceWirePilotCodec.RelocationUserSpotIdentity(
                                "matrix-room",
                                spot.objectGeneration(),
                                spot.authorityOwnerGeneration()),
                        1,
                        List.of(
                                new ServiceWirePilotCodec.RelocationApplicationState(
                                        1, false, new byte[0]),
                                new ServiceWirePilotCodec.RelocationApplicationState(
                                        2, false, new byte[0])),
                        List.of(),
                        List.of(),
                        List.of(),
                        List.of()));
    }

    private static ZLinkLocationStore recordingProvider(
            ZLinkRedisLocationStore provider,
            List<ZLinkStoreWriteRequest> writes,
            AtomicBoolean loseNextWriteResponse) {
        return (ZLinkLocationStore)
                Proxy.newProxyInstance(
                        ZLinkLocationStore.class.getClassLoader(),
                        new Class<?>[] {ZLinkLocationStore.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("write"))
                                writes.add((ZLinkStoreWriteRequest) args[0]);
                            try {
                                Object result = method.invoke(provider, args);
                                if (method.getName().equals("write")
                                        && loseNextWriteResponse.compareAndSet(true, false)) {
                                    return ((CompletionStage<?>) result)
                                            .thenCompose(
                                                    ignored ->
                                                            CompletableFuture.failedFuture(
                                                                    new IllegalStateException(
                                                                            "response lost after write")));
                                }
                                return result;
                            } catch (InvocationTargetException failure) {
                                throw failure.getCause();
                            }
                        });
    }

    private static systems.zlink.framework.locationprovider.ZLinkStoreValue read(
            ZLinkRedisLocationStore provider, ZLinkStoreKey key) {
        return assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(key, OPEN::isCancellationRequested)
                                .toCompletableFuture()
                                .join())
                .value();
    }

    private static ZLinkLocationOwnerToken claim(ZLinkLocationRepository repository, String owner) {
        return assertInstanceOf(
                        ZLinkOwnerLeaseClaimed.class,
                        repository
                                .claimOwnerLease(owner, Duration.ofMinutes(1))
                                .toCompletableFuture()
                                .join())
                .token();
    }

    private static void abortPreparedInventory(
            ZLinkLocationRepository repository,
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkAggregateFence fence,
            ZLinkAggregateRelocationCoordinator.ExpectedParticipant primary,
            ZLinkAuthoritySnapshot captured,
            List<ZLinkAggregateRelocationCoordinator.SealedParticipant> inventory,
            ZLinkMeshNodeDescriptor node,
            ZLinkLocationOwnerToken owner) {
        var record = captured.relocation().json();
        record.put("phase", "prepared");
        record.putObject("target")
                .put("nodeRid", node.rid().toHex())
                .put("nodeGeneration", Long.toUnsignedString(node.lifecycleGeneration()))
                .put("ownerId", owner.ownerId())
                .put("ownerLeaseGeneration", Long.toUnsignedString(owner.leaseGeneration()))
                .put("attemptGeneration", "1");
        var prepared = new ZLinkAuthorityRelocation(record);
        var participants =
                inventory.stream()
                        .map(
                                entry -> {
                                    var snapshot = entry.authority().snapshot();
                                    return new ZLinkAggregateParticipant(
                                            entry.authority().key(),
                                            snapshot.objectGeneration(),
                                            snapshot.authorityOwnerGeneration(),
                                            entry.expectedStoreVersion(),
                                            entry.ownerTransition(),
                                            entry.authorityPayload(),
                                            entry.membershipMutation(),
                                            prepared,
                                            snapshot.storeVersion());
                                })
                        .toList();
        var request =
                new ZLinkAggregatePrepareRequest(
                        fence.aggregateId(),
                        fence.aggregateGeneration(),
                        participants,
                        new ZLinkMeshNodeDescriptorKey(node.meshName(), node.rid()),
                        node.lifecycleGeneration(),
                        new ZLinkPlacementCapacityBundle(
                                1,
                                1,
                                Optional.of(
                                        new ZLinkSpotTypeCapacityDelta(
                                                ZLinkPlacementObjectKind.USER_SPOT, "type", 1))),
                        owner);
        assertInstanceOf(
                ZLinkAggregatePrepared.class,
                repository.prepareAggregate(request, OPEN).toCompletableFuture().join());
        assertEquals("prepared", snapshot(repository, ACTOR).relocation().phase());
        coordinator
                .abortSource(primary, fence, owner.ownerId(), owner.leaseGeneration(), OPEN)
                .toCompletableFuture()
                .join();
        assertNull(snapshot(repository, ACTOR).relocation());
        assertEquals("aborted", snapshot(repository, SPOT).relocation().phase());
    }

    private static ZLinkAuthoritySnapshot activate(
            ZLinkLocationRepository repository,
            ZLinkMeshNodeDescriptor node,
            ZLinkLocationOwnerToken owner,
            String key,
            ZLinkPlacementObjectKind kind) {
        return activate(repository, node, owner, key, kind, new byte[] {1});
    }

    private static ZLinkAuthoritySnapshot activate(
            ZLinkLocationRepository repository,
            ZLinkMeshNodeDescriptor node,
            ZLinkLocationOwnerToken owner,
            String key,
            ZLinkPlacementObjectKind kind,
            byte[] payload) {
        var request =
                new ZLinkObjectReservationRequest(
                        kind,
                        key,
                        "type",
                        "inline-v1:test",
                        new byte[32],
                        4,
                        new ZLinkMeshNodeDescriptorKey(node.meshName(), node.rid()),
                        node.lifecycleGeneration(),
                        owner,
                        payload,
                        kind == ZLinkPlacementObjectKind.ACTOR
                                ? ZLinkPlacementCapacityBundle.actor(1)
                                : ZLinkPlacementCapacityBundle.spot(kind, "type", 1));
        var reservation =
                assertInstanceOf(
                        ZLinkObjectReserved.class,
                        repository.reserve(request, OPEN).toCompletableFuture().join());
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(reservation.reservation(), payload, OPEN)
                        .toCompletableFuture()
                        .join());
        return snapshot(repository, key);
    }

    private static ZLinkAuthoritySnapshot snapshot(ZLinkLocationRepository repository, String key) {
        return assertInstanceOf(
                ZLinkAuthoritySnapshot.class,
                repository.read(key, OPEN).toCompletableFuture().join());
    }

    private static ZLinkMeshNodeDescriptor descriptor(ZLinkLocationOwnerToken owner) {
        return new ZLinkMeshNodeDescriptor(
                "game",
                RoutingId.from(owner.ownerId()),
                owner.ownerId().equals("source") ? 1 : 2,
                1,
                "tcp://127.0.0.1:7100",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "type",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                0),
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                "type",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                0)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("entry-source"),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(0, 0, 0),
                        new ZLinkCapacityUsage(0, 0, 0),
                        List.of()),
                new ZLinkActivationConcurrency(0, 64),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.now());
    }
}
