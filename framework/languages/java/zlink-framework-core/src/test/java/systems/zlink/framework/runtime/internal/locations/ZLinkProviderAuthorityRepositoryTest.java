package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreCancellation;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreScanResult;
import systems.zlink.framework.locationprovider.ZLinkStoreValue;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteResult;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.io.IOException;
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
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.regex.Pattern;
import java.util.stream.IntStream;

final class ZLinkProviderAuthorityRepositoryTest {
    private record ReclaimActor(systems.zlink.framework.actors.ZLinkActorContext context)
            implements systems.zlink.framework.actors.ZLinkActor {}

    private static final class ReclaimFactory
            implements systems.zlink.framework.actors.ZLinkActorFactory {
        public CompletionStage<systems.zlink.framework.actors.ZLinkActor> create(
                systems.zlink.framework.actors.ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ReclaimActor(context));
        }
    }

    @ParameterizedTest
    @org.junit.jupiter.params.provider.CsvSource({
        "expired,false",
        "expired,true",
        "live,false",
        "live,true",
        "liveDescriptorGone,false",
        "liveDescriptorGone,true",
        "recreate,false",
        "recreate,true",
        "typeMismatch,false",
        "typeMismatch,true",
        "race,false",
        "race,true",
        "capacityRace,false",
        "relocation,false",
        "relocation,true",
        "relocationRecreate,false",
        "relocationRecreate,true"
    })
    void endedActiveActorRecreation(String scenario, boolean inMemory) throws Exception {
        var provider = new TemporaryCommitConflictStore(new ZLinkInMemoryProviderLocationStore());
        ZLinkLocationRepository repository =
                inMemory
                        ? new systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore()
                        : new ZLinkProviderLocationRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                repository
                                        .claimOwnerLease("reclaim-old", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();

        var descriptor = actorReclaimDescriptor(owner, 1);
        repository
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();

        var key = ZLinkAuthorityKeyCodec.actor("reclaim-actor");
        var registration =
                new systems.zlink.framework.runtime.mesh.MeshNodeRegistration("reclaim-source");
        registration
                .objects()
                .server()
                .addActorFactory(
                        "player",
                        ReclaimActor.class,
                        ReclaimFactory.class,
                        factory -> factory.disableRelocation());
        var policy = registration.actorRelocationPolicy("player");
        var request =
                new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.ACTOR,
                        key,
                        "player",
                        "inline:test",
                        new byte[32],
                        1,
                        new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid()),
                        descriptor.lifecycleGeneration(),
                        owner,
                        new byte[] {1},
                        ZLinkPlacementCapacityBundle.actor(1),
                        policy);
        var first =
                ((ZLinkObjectReserved)
                                repository
                                        .reserve(request, () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        var terminal =
                new ZLinkCreationOperationTerminal(
                        new ZLinkCreationOperationIdentity(
                                RoutingId.from("reclaim-source"), 1, 1, 1),
                        first,
                        ZLinkCreationTerminalState.CREATED,
                        new byte[] {1},
                        Instant.now().plusSeconds(300));
        byte[] readyPayload = new byte[] {9};
        if (scenario.equals("relocation") || scenario.equals("relocationRecreate")) {
            byte[] canonical =
                    new ZLinkActorAuthorityPayloadCodec()
                            .encode(
                                    ZLinkActorAuthorityPayloadCodec.State.READY,
                                    "player",
                                    "reclaim-actor",
                                    "reclaim-entry",
                                    1,
                                    1,
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    descriptor.meshName(),
                                    descriptor.rid(),
                                    1);
            var relocation =
                    new ZLinkAggregateRelocationCoordinator.Request(
                            new UUID(0, 9),
                            1,
                            2,
                            List.of(
                                    new ZLinkAggregateRelocationCoordinator.Participant(
                                            key,
                                            ZLinkPlacementObjectKind.ACTOR,
                                            1,
                                            1,
                                            "1",
                                            ZLinkAuthorityGenerationTransition.PRESERVE,
                                            canonical,
                                            new byte[0])),
                            goldenRoot(),
                            request.targetDescriptor(),
                            1,
                            ZLinkPlacementCapacityBundle.actor(1),
                            owner,
                            "1");
            readyPayload =
                    ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                            canonical, relocation, ZLinkAuthorityGenerationTransition.PRESERVE);
            assertTrue(ZLinkCanonicalRelocationAuthorityStateCodec.decode(readyPayload) != null);
        }
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(first, readyPayload, terminal, () -> false)
                        .toCompletableFuture()
                        .get());
        if (scenario.equals("liveDescriptorGone"))
            repository
                    .removeMeshNode(request.targetDescriptor(), owner)
                    .toCompletableFuture()
                    .get();
        if (!scenario.equals("live") && !scenario.equals("liveDescriptorGone"))
            repository.releaseOwnerLease(owner).toCompletableFuture().get();
        var newOwner =
                ((ZLinkOwnerLeaseClaimed)
                                repository
                                        .claimOwnerLease("reclaim-new", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var nextDescriptor = actorReclaimDescriptor(newOwner, 2);
        repository
                .updateMeshNode(nextDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var next =
                new ZLinkObjectReservationRequest(
                        ZLinkPlacementObjectKind.ACTOR,
                        key,
                        scenario.equals("typeMismatch") ? "different" : "player",
                        "inline:new",
                        new byte[32],
                        1,
                        new ZLinkMeshNodeDescriptorKey(
                                nextDescriptor.meshName(), nextDescriptor.rid()),
                        nextDescriptor.lifecycleGeneration(),
                        newOwner,
                        new byte[] {2},
                        ZLinkPlacementCapacityBundle.actor(1),
                        (scenario.equals("recreate") || scenario.equals("relocationRecreate"))
                                ? new systems.zlink.framework.runtime.internal.configuration
                                        .ZLinkObjectFactoryRegistration.RelocationPolicy.Recreate()
                                : policy);
        var retainedBefore = repository.read(key, () -> false).toCompletableFuture().get();
        if (scenario.equals("recreate")
                || scenario.equals("relocation")
                || scenario.equals("relocationRecreate")) {
            var failure =
                    inMemory
                            ? assertThrows(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    () -> repository.reserve(next, () -> false))
                            : assertInstanceOf(
                                    systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                    assertThrows(
                                                    java.util.concurrent.ExecutionException.class,
                                                    () ->
                                                            repository
                                                                    .reserve(next, () -> false)
                                                                    .toCompletableFuture()
                                                                    .get())
                                            .getCause());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                    failure.kind());
            var before = assertInstanceOf(ZLinkAuthoritySnapshot.class, retainedBefore);
            var after =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(key, () -> false).toCompletableFuture().get());
            assertEquals(before.storeVersion(), after.storeVersion());
            assertEquals(before.objectGeneration(), after.objectGeneration());
            assertArrayEquals(readyPayload, after.payload());
        } else if (scenario.equals("capacityRace")) {
            var winner =
                    new java.util.concurrent.atomic.AtomicReference<ZLinkObjectReserveResult>();
            provider.beforeRead =
                    storeKey -> {
                        if (!storeKey.value().contains("capacity:"))
                            return CompletableFuture.completedFuture(null);
                        provider.beforeRead = null;
                        return repository.reserve(next, () -> false).thenAccept(winner::set);
                    };
            var loser = repository.reserve(next, () -> false).toCompletableFuture().get();
            assertInstanceOf(ZLinkObjectReserved.class, winner.get());
            assertInstanceOf(ZLinkObjectConflict.class, loser);
        } else if (scenario.equals("race")) {
            var a = repository.reserve(next, () -> false).toCompletableFuture();
            var b = repository.reserve(next, () -> false).toCompletableFuture();
            assertEquals(
                    1,
                    java.util.stream.Stream.of(a.get(), b.get())
                            .filter(ZLinkObjectReserved.class::isInstance)
                            .count());
        } else {
            var result = repository.reserve(next, () -> false).toCompletableFuture().get();
            if (scenario.equals("live") || scenario.equals("liveDescriptorGone"))
                assertInstanceOf(ZLinkObjectAlreadyExists.class, result);
            else if (scenario.equals("typeMismatch"))
                assertInstanceOf(ZLinkObjectTypeMismatch.class, result);
            else {
                var reserved = assertInstanceOf(ZLinkObjectReserved.class, result).reservation();
                assertTrue(reserved.objectGeneration() > first.objectGeneration());
                assertArrayEquals(
                        next.creatingPayload(),
                        ((ZLinkAuthoritySnapshot)
                                        repository
                                                .read(key, () -> false)
                                                .toCompletableFuture()
                                                .get())
                                .payload());
            }
        }
    }

    private static ZLinkMeshNodeDescriptor actorReclaimDescriptor(
            ZLinkLocationOwnerToken owner, long generation) {
        var d = capacityDescriptor(owner);
        return new ZLinkMeshNodeDescriptor(
                d.meshName(),
                RoutingId.from("reclaim-node-" + generation),
                generation,
                d.descriptorRevision(),
                d.endpoint(),
                d.channelWeights(),
                d.applicationVersion(),
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                0)),
                d.objectRole(),
                Optional.of("reclaim-entry-" + generation),
                d.placementWeight(),
                d.capacity(),
                d.activationConcurrency(),
                d.maintenanceWave(),
                d.state(),
                d.securityIdentity(),
                d.ownerId(),
                d.leaseGeneration(),
                d.updatedAt());
    }

    private static final String OBJECT_COUNTER_KEY = "zlink:v11:object-counter";
    private static final String AUTHORITY_OWNER_COUNTER_KEY = "zlink:v11:authority-owner-counter";

    @ParameterizedTest
    @ValueSource(ints = {10, 16384})
    void deleteRebuildsCapacityConditionsAfterProviderConflict(int conflicts) throws Exception {
        var fixture = reincarnationFixture("delete-capacity-conflict");
        fixture.repository()
                .commit(fixture.reservation(), new byte[] {2}, null, () -> false)
                .toCompletableFuture()
                .get();
        var before =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        var attempts = new int[1];
        var provider =
                new ZLinkLocationStore() {
                    @Override
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                        return fixture.provider().read(key, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
                        if (request.mutations().stream()
                                .anyMatch(ZLinkStoreDelete.class::isInstance)) {
                            attempts[0]++;
                            if (attempts[0] <= conflicts) {
                                var capacityKey =
                                        request.mutations().stream()
                                                .filter(ZLinkStorePut.class::isInstance)
                                                .map(ZLinkStorePut.class::cast)
                                                .map(ZLinkStorePut::key)
                                                .findFirst()
                                                .orElseThrow();
                                return read(capacityKey, cancellation)
                                        .thenCompose(
                                                read -> {
                                                    var found =
                                                            assertInstanceOf(
                                                                    ZLinkStoreReadFound.class,
                                                                    read);
                                                    return fixture.provider()
                                                            .write(
                                                                    new ZLinkStoreWriteRequest(
                                                                            List.of(),
                                                                            List.of(
                                                                                    new ZLinkStorePut(
                                                                                            capacityKey,
                                                                                            found.value()
                                                                                                    .bytes(),
                                                                                            null))),
                                                                    cancellation);
                                                })
                                        .thenCompose(
                                                ignored ->
                                                        fixture.provider()
                                                                .write(request, cancellation));
                            }
                        }
                        return fixture.provider().write(request, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
                        return fixture.provider().scan(request, cancellation);
                    }
                };
        assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                new ZLinkProviderAuthorityRepository(provider)
                        .compareExchange(
                                fixture.key(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityDelete(),
                                () -> false)
                        .toCompletableFuture()
                        .get());
        assertEquals(conflicts + 1, attempts[0]);
        assertInstanceOf(
                ZLinkAuthorityMissing.class,
                fixture.repository().read(fixture.key(), () -> false).toCompletableFuture().get());
    }

    @ParameterizedTest
    @ValueSource(strings = {"live", "expired", "missingCapacity", "underflow"})
    void endedReservationReleasePreservesCapacity(String scenario) throws Exception {
        var now =
                new java.util.concurrent.atomic.AtomicReference<>(
                        Instant.parse("2026-10-07T00:00:00Z"));
        var clock =
                new java.time.Clock() {
                    public java.time.ZoneId getZone() {
                        return java.time.ZoneOffset.UTC;
                    }

                    public java.time.Clock withZone(java.time.ZoneId zone) {
                        return this;
                    }

                    public Instant instant() {
                        return now.get();
                    }
                };
        var provider = new ZLinkInMemoryProviderLocationStore(clock);
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("release-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var key = ZLinkAuthorityKeyCodec.spot("release-reservation");
        var reservation =
                ((ZLinkObjectReserved)
                                repository
                                        .reserve(
                                                capacityRequest(key, descriptor, owner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        var capacityKey = new ZLinkStoreKey("zlink:v11:capacity:game:capacity-node");
        if (scenario.equals("missingCapacity") || scenario.equals("underflow")) {
            provider.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            scenario.equals("missingCapacity")
                                                    ? new ZLinkStoreDelete(capacityKey)
                                                    : new ZLinkStorePut(
                                                            capacityKey,
                                                            "{\"active\":{\"actors\":0,\"spots\":0,\"spotTypes\":{}},\"pending\":{\"actors\":0,\"spots\":0,\"spotTypes\":{}}}"
                                                                    .getBytes(
                                                                            java.nio.charset
                                                                                    .StandardCharsets
                                                                                    .UTF_8),
                                                            null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
        }
        if (scenario.equals("expired")) now.set(now.get().plusSeconds(61));
        else if (!scenario.equals("live")) owners.release(owner).toCompletableFuture().get();
        if (scenario.equals("underflow")) {
            var capacityBefore =
                    (ZLinkStoreReadFound)
                            provider.read(capacityKey, () -> false).toCompletableFuture().get();
            var failure =
                    assertThrows(
                            java.util.concurrent.ExecutionException.class,
                            () ->
                                    repository
                                            .releaseEndedReservation(
                                                    key, reservation.storeVersion(), () -> false)
                                            .toCompletableFuture()
                                            .get());
            assertInstanceOf(IllegalStateException.class, failure.getCause());
            assertTrue(failure.getCause().getMessage().contains("capacity record is inconsistent"));
            var capacityAfter =
                    (ZLinkStoreReadFound)
                            provider.read(capacityKey, () -> false).toCompletableFuture().get();
            assertEquals(capacityBefore.value().version(), capacityAfter.value().version());
            assertArrayEquals(capacityBefore.value().bytes(), capacityAfter.value().bytes());
            assertEquals(
                    reservation.storeVersion(),
                    ((ZLinkAuthoritySnapshot)
                                    repository.read(key, () -> false).toCompletableFuture().get())
                            .storeVersion());
        } else {
            assertEquals(
                    !scenario.equals("live"),
                    repository
                            .releaseEndedReservation(key, reservation.storeVersion(), () -> false)
                            .toCompletableFuture()
                            .get());
            if (scenario.equals("live"))
                assertEquals(
                        ZLinkObjectCommitResult.COMMITTED,
                        repository
                                .commit(reservation, new byte[] {1}, null, () -> false)
                                .toCompletableFuture()
                                .get());
            else
                assertInstanceOf(
                        ZLinkAuthorityMissing.class,
                        repository.read(key, () -> false).toCompletableFuture().get());
            if (!scenario.equals("live"))
                assertEquals(
                        ZLinkObjectCommitResult.STALE,
                        repository
                                .commit(reservation, new byte[] {1}, null, () -> false)
                                .toCompletableFuture()
                                .get());
            if (scenario.equals("missingCapacity"))
                assertInstanceOf(
                        systems.zlink.framework.locationprovider.ZLinkStoreReadMissing.class,
                        provider.read(capacityKey, () -> false).toCompletableFuture().get());
        }
    }

    @Test
    void startupReleasesPreviousLifecycleBeforeDeletingRecoveryRoot() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("startup-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = startupDescriptor(owner, 1);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var repository = new ZLinkProviderLocationRepository(provider);
        var key = ZLinkAuthorityKeyCodec.spot("startup-reservation");
        var codec =
                new systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec();
        var payload =
                codec.encode(
                        new systems.zlink.framework.runtime.locations
                                .ZLinkServiceAuthorityPayloadCodec.InstanceSpotAuthority(
                                systems.zlink.framework.runtime.locations
                                        .ZLinkServiceAuthorityPayloadCodec.State.CREATING,
                                "room",
                                "startup-reservation",
                                owner.ownerId(),
                                owner.leaseGeneration(),
                                descriptor.meshName(),
                                descriptor.rid(),
                                1));
        var reserved =
                ((ZLinkObjectReserved)
                                repository
                                        .reserve(
                                                new ZLinkObjectReservationRequest(
                                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                                        key,
                                                        "room",
                                                        "startup-root",
                                                        new byte[32],
                                                        4,
                                                        new ZLinkMeshNodeDescriptorKey(
                                                                descriptor.meshName(),
                                                                descriptor.rid()),
                                                        1,
                                                        owner,
                                                        payload,
                                                        ZLinkPlacementCapacityBundle.spot(
                                                                ZLinkPlacementObjectKind
                                                                        .INSTANCE_SPOT,
                                                                "room",
                                                                1)),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        descriptors.removeMeshNode(reserved.targetDescriptor(), owner).toCompletableFuture().get();
        descriptors
                .updateMeshNode(startupDescriptor(owner, 2), ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var rootExists = new java.util.concurrent.atomic.AtomicBoolean(true);
        var node =
                (systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode)
                        java.lang.reflect.Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkInternalMeshNode.class
                                },
                                (proxy, method, args) ->
                                        switch (method.getName()) {
                                            case "routingId" -> descriptor.rid();
                                            case "lifecycleGeneration" -> 2L;
                                            default ->
                                                    throw new AssertionError(
                                                            "Unexpected startup mesh operation: "
                                                                    + method.getName());
                                        });
        var roots =
                (ZLinkRelocationStore)
                        java.lang.reflect.Proxy.newProxyInstance(
                                getClass().getClassLoader(),
                                new Class<?>[] {ZLinkRelocationStore.class},
                                (proxy, method, args) -> {
                                    assertEquals("delete", method.getName());
                                    assertEquals("startup-root", args[0]);
                                    assertInstanceOf(
                                            ZLinkAuthorityMissing.class,
                                            repository
                                                    .read(key, () -> false)
                                                    .toCompletableFuture()
                                                    .get());
                                    var row =
                                            (ZLinkStoreReadFound)
                                                    provider.read(
                                                                    new ZLinkStoreKey(
                                                                            "zlink:v11:capacity:game:capacity-node"),
                                                                    () -> false)
                                                            .toCompletableFuture()
                                                            .get();
                                    assertTrue(
                                            new String(
                                                            row.value().bytes(),
                                                            java.nio.charset.StandardCharsets.UTF_8)
                                                    .contains(
                                                            "\"pending\":{\"actors\":0,\"spots\":0"));
                                    assertTrue(rootExists.compareAndSet(true, false));
                                    return CompletableFuture.completedFuture(
                                            ZLinkRelocationDeleteResult.DELETED);
                                });
        try (var runtime =
                new systems.zlink.framework.runtime.locations.ZLinkStatefulAuthorityRouteRuntime(
                        repository,
                        roots,
                        Map.of("game", node),
                        Duration.ofHours(1),
                        failure -> {
                            throw new AssertionError(failure);
                        })) {
            assertTrue(rootExists.get());
            runtime.start().toCompletableFuture().get();
            assertFalse(rootExists.get());
        }
    }

    private static ZLinkMeshNodeDescriptor startupDescriptor(
            ZLinkLocationOwnerToken owner, long generation) {
        var d = capacityDescriptor(owner);
        return new ZLinkMeshNodeDescriptor(
                d.meshName(),
                d.rid(),
                generation,
                d.descriptorRevision(),
                d.endpoint(),
                d.channelWeights(),
                d.applicationVersion(),
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                "room",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                1)),
                d.objectRole(),
                d.entrySpotId(),
                d.placementWeight(),
                d.capacity(),
                d.activationConcurrency(),
                d.maintenanceWave(),
                d.state(),
                d.securityIdentity(),
                d.ownerId(),
                d.leaseGeneration(),
                d.updatedAt());
    }

    @Test
    void lateCommitWinsAgainstReservationReleaseBatch() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("race-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var writer = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var key = ZLinkAuthorityKeyCodec.spot("release-race");
        var reservation =
                ((ZLinkObjectReserved)
                                writer.reserve(capacityRequest(key, descriptor, owner), () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        descriptors
                .removeMeshNode(reservation.targetDescriptor(), owner)
                .toCompletableFuture()
                .get();
        var committed = new java.util.concurrent.atomic.AtomicBoolean();
        ZLinkLocationStore raced =
                new ZLinkLocationStore() {
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                        return provider.read(key, cancellation);
                    }

                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
                        return provider.scan(request, cancellation);
                    }

                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
                        assertTrue(committed.compareAndSet(false, true));
                        return descriptors
                                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                                .thenCompose(
                                        ignored ->
                                                writer.commit(
                                                        reservation,
                                                        new byte[] {1},
                                                        null,
                                                        () -> false))
                                .thenCompose(
                                        result -> {
                                            assertEquals(ZLinkObjectCommitResult.COMMITTED, result);
                                            return provider.write(request, cancellation);
                                        });
                    }
                };
        assertFalse(
                new ZLinkProviderAuthorityRepository(raced, descriptors)
                        .releaseEndedReservation(key, reservation.storeVersion(), () -> false)
                        .toCompletableFuture()
                        .get());
        assertTrue(committed.get());
    }

    @Test
    void missingDescriptorReclaimsReservedAuthorityWithLiveOwner() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("ended-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var key = ZLinkAuthorityKeyCodec.spot("ended-reservation");
        var request = capacityRequest(key, descriptor, owner);
        assertInstanceOf(
                ZLinkObjectReserved.class,
                repository.reserve(request, () -> false).toCompletableFuture().get());
        descriptors.removeMeshNode(request.targetDescriptor(), owner).toCompletableFuture().get();

        var replacement = capacityDescriptor(owner, "game", RoutingId.from("replacement-node"));
        descriptors
                .updateMeshNode(replacement, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        assertInstanceOf(
                ZLinkObjectReserved.class,
                repository
                        .reserve(capacityRequest(key, replacement, owner), () -> false)
                        .toCompletableFuture()
                        .get());
    }

    @Test
    void reservedAuthorityRejectsPreserveDeleteAndReincarnateWithoutMutation() throws Exception {
        var fixture = reincarnationFixture("reserved-close");
        var before =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        for (var mutation :
                List.of(
                        new ZLinkAuthorityPut(new byte[] {3}),
                        new ZLinkAuthorityDelete(),
                        ZLinkAuthorityMutation.reincarnate(new byte[] {4}))) {
            assertInstanceOf(
                    ZLinkAuthorityConflict.class,
                    fixture.repository()
                            .compareExchange(
                                    fixture.key(),
                                    new ZLinkAuthorityExpectFound(before.storeVersion()),
                                    mutation,
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            var after =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            fixture.repository()
                                    .read(fixture.key(), () -> false)
                                    .toCompletableFuture()
                                    .get());
            assertEquals(before.storeVersion(), after.storeVersion());
            assertArrayEquals(before.payload(), after.payload());
            assertEquals(before.allocation(), after.allocation());
            assertCounterValue(fixture.provider(), OBJECT_COUNTER_KEY, "2");
            assertCounterValue(fixture.provider(), AUTHORITY_OWNER_COUNTER_KEY, "2");
        }
    }

    @Test
    void reincarnateIssuesBothGenerationsAndPreservesOwnerAndActiveCapacity() throws Exception {
        var fixture = reincarnationFixture("active-close");
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                fixture.repository()
                        .commit(fixture.reservation(), new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        var before =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        var after =
                assertInstanceOf(
                        ZLinkAuthorityStored.class,
                        fixture.repository()
                                .compareExchange(
                                        fixture.key(),
                                        new ZLinkAuthorityExpectFound(before.storeVersion()),
                                        ZLinkAuthorityMutation.reincarnate(new byte[] {3}),
                                        () -> false)
                                .toCompletableFuture()
                                .get());
        assertTrue(after.objectGeneration() > before.objectGeneration());
        assertTrue(after.authorityOwnerGeneration() > before.authorityOwnerGeneration());
        assertTrue(!after.storeVersion().equals(before.storeVersion()));
        assertEquals(before.ownerId(), after.ownerId());
        assertEquals(before.ownerLeaseGeneration(), after.ownerLeaseGeneration());
        assertEquals(before.allocation(), after.allocation());
        assertArrayEquals(new byte[] {3}, after.payload());
        assertCounterValue(fixture.provider(), OBJECT_COUNTER_KEY, "3");
        assertCounterValue(fixture.provider(), AUTHORITY_OWNER_COUNTER_KEY, "3");
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                fixture.repository()
                        .compareExchange(
                                fixture.key(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                new ZLinkAuthorityDelete(),
                                () -> false)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                fixture.repository()
                        .reserve(
                                capacityRequest(
                                        ZLinkAuthorityKeyCodec.spot("other-close"),
                                        fixture.descriptor(),
                                        fixture.reservation().targetOwner()),
                                () -> false)
                        .toCompletableFuture()
                        .get());
        var visible =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        assertEquals(after.storeVersion(), visible.storeVersion());
        assertEquals(after.objectGeneration(), visible.objectGeneration());
    }

    @Test
    void reincarnateCounterExhaustionLeavesBothCountersAndAuthorityUnchanged() throws Exception {
        for (String exhausted : List.of(OBJECT_COUNTER_KEY, AUTHORITY_OWNER_COUNTER_KEY)) {
            var fixture = reincarnationFixture("exhausted-close");
            fixture.repository()
                    .commit(fixture.reservation(), new byte[] {2}, null, () -> false)
                    .toCompletableFuture()
                    .get();
            var before =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            fixture.repository()
                                    .read(fixture.key(), () -> false)
                                    .toCompletableFuture()
                                    .get());
            fixture.provider()
                    .write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(
                                                    new ZLinkStoreKey(exhausted),
                                                    Long.toString(Long.MAX_VALUE)
                                                            .getBytes(
                                                                    java.nio.charset
                                                                            .StandardCharsets
                                                                            .UTF_8),
                                                    null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkAuthorityGenerationExhausted.class,
                    fixture.repository()
                            .compareExchange(
                                    fixture.key(),
                                    new ZLinkAuthorityExpectFound(before.storeVersion()),
                                    ZLinkAuthorityMutation.reincarnate(new byte[] {3}),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            var after =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            fixture.repository()
                                    .read(fixture.key(), () -> false)
                                    .toCompletableFuture()
                                    .get());
            assertEquals(before.storeVersion(), after.storeVersion());
            assertArrayEquals(before.payload(), after.payload());
            for (String counter : List.of(OBJECT_COUNTER_KEY, AUTHORITY_OWNER_COUNTER_KEY)) {
                assertCounterValue(
                        fixture.provider(),
                        counter,
                        counter.equals(exhausted) ? Long.toString(Long.MAX_VALUE) : "2");
            }
        }
    }

    private static ReincarnationFixture reincarnationFixture(String id) throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                owners.claim(id, Duration.ofMinutes(1)).toCompletableFuture().get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        String key = ZLinkAuthorityKeyCodec.spot(id);
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(key, descriptor, owner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        return new ReincarnationFixture(provider, repository, descriptor, key, reservation);
    }

    @Test
    void reincarnateRebuildsBothCountersAfterProviderConflict() throws Exception {
        for (String counterKey : List.of(OBJECT_COUNTER_KEY, AUTHORITY_OWNER_COUNTER_KEY)) {
            var fixture = reincarnationFixture("contended-close");
            fixture.repository()
                    .commit(fixture.reservation(), new byte[] {2}, null, () -> false)
                    .toCompletableFuture()
                    .get();
            var before =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            fixture.repository()
                                    .read(fixture.key(), () -> false)
                                    .toCompletableFuture()
                                    .get());
            var identity = ZLinkAuthorityKeyCodec.decode(fixture.key());
            var authorityKey = ZLinkOpaqueRecordKey.of("authority", identity.kind(), identity.id());
            var contended =
                    new IndependentCounterContentionStore(
                            fixture.provider(),
                            authorityKey,
                            "7",
                            null,
                            new ZLinkStoreKey(counterKey));
            var repository = new ZLinkProviderAuthorityRepository(contended);
            var stored =
                    assertInstanceOf(
                            ZLinkAuthorityStored.class,
                            repository
                                    .compareExchange(
                                            fixture.key(),
                                            new ZLinkAuthorityExpectFound(before.storeVersion()),
                                            ZLinkAuthorityMutation.reincarnate(new byte[] {3}),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            assertTrue(contended.bumpedCounter);
            for (String key : List.of(OBJECT_COUNTER_KEY, AUTHORITY_OWNER_COUNTER_KEY)) {
                assertCounterValue(fixture.provider(), key, key.equals(counterKey) ? "8" : "3");
            }
            var after =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            fixture.repository()
                                    .read(fixture.key(), () -> false)
                                    .toCompletableFuture()
                                    .get());
            assertEquals(stored.storeVersion(), after.storeVersion());
            assertArrayEquals(new byte[] {3}, after.payload());
            assertEquals(counterKey.equals(OBJECT_COUNTER_KEY) ? 7 : 2, after.objectGeneration());
            assertEquals(
                    counterKey.equals(AUTHORITY_OWNER_COUNTER_KEY) ? 7 : 2,
                    after.authorityOwnerGeneration());
        }
    }

    @Test
    void reincarnateRequiresTheCurrentOwnerLeaseWithoutChangingCounters() throws Exception {
        var fixture = reincarnationFixture("released-close");
        fixture.repository()
                .commit(fixture.reservation(), new byte[] {2}, null, () -> false)
                .toCompletableFuture()
                .get();
        var before =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        new ZLinkProviderOwnerLeaseRepository(fixture.provider())
                .release(fixture.reservation().targetOwner())
                .toCompletableFuture()
                .get();
        assertInstanceOf(
                ZLinkAuthorityConflict.class,
                fixture.repository()
                        .compareExchange(
                                fixture.key(),
                                new ZLinkAuthorityExpectFound(before.storeVersion()),
                                ZLinkAuthorityMutation.reincarnate(new byte[] {3}),
                                () -> false)
                        .toCompletableFuture()
                        .get());
        assertCounterValue(fixture.provider(), OBJECT_COUNTER_KEY, "2");
        assertCounterValue(fixture.provider(), AUTHORITY_OWNER_COUNTER_KEY, "2");
        var after =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        fixture.repository()
                                .read(fixture.key(), () -> false)
                                .toCompletableFuture()
                                .get());
        assertEquals(before.storeVersion(), after.storeVersion());
        assertArrayEquals(before.payload(), after.payload());
    }

    private record ReincarnationFixture(
            ZLinkInMemoryProviderLocationStore provider,
            ZLinkProviderAuthorityRepository repository,
            ZLinkMeshNodeDescriptor descriptor,
            String key,
            ZLinkObjectReservation reservation) {}

    @Test
    void authorityParticipantMarkersRoundTripNodeWireBytesThroughPublicRepository()
            throws Exception {
        for (var transition : ZLinkAuthorityGenerationTransition.values()) {
            var provider = new ZLinkInMemoryProviderLocationStore();
            var repository = new ZLinkProviderLocationRepository(provider);
            var owner =
                    assertInstanceOf(
                                    ZLinkOwnerLeaseClaimed.class,
                                    repository
                                            .claimOwnerLease("marker-owner", Duration.ofHours(1))
                                            .toCompletableFuture()
                                            .join())
                            .token();
            var descriptor = capacityDescriptor(owner);
            assertEquals(
                    ZLinkLocationWriteStatus.STORED,
                    repository
                            .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                            .toCompletableFuture()
                            .join()
                            .status());
            String key = ZLinkAuthorityKeyCodec.spot("marker-spot");
            var reservation =
                    assertInstanceOf(
                                    ZLinkObjectReserved.class,
                                    repository
                                            .reserve(
                                                    capacityRequest(key, descriptor, owner),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .join())
                            .reservation();
            byte[] payload =
                    new systems.zlink.framework.runtime.locations
                                    .ZLinkServiceAuthorityPayloadCodec()
                            .encodeUser(
                                    systems.zlink.framework.runtime.locations
                                            .ZLinkServiceAuthorityPayloadCodec.State.READY,
                                    "room",
                                    "marker-spot",
                                    owner.ownerId(),
                                    owner.leaseGeneration(),
                                    descriptor.meshName(),
                                    descriptor.rid(),
                                    descriptor.lifecycleGeneration());
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    repository
                            .commit(reservation, payload, null, () -> false)
                            .toCompletableFuture()
                            .join());
            var current =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(key, () -> false).toCompletableFuture().join());
            var rowKey = authorityKey(key);
            var mapper = new com.fasterxml.jackson.databind.ObjectMapper();
            var base =
                    mapper.readTree(
                            assertInstanceOf(
                                            ZLinkStoreReadFound.class,
                                            provider.read(rowKey, () -> false)
                                                    .toCompletableFuture()
                                                    .join())
                                    .value()
                                    .bytes());
            byte[] root = goldenRoot();
            var envelope = ZLinkServiceRelocationEnvelopeCodec.decode(root);
            var request =
                    new ZLinkAggregateRelocationCoordinator.Request(
                            new UUID(envelope.relocationHigh(), envelope.relocationLow()),
                            1,
                            1,
                            List.of(
                                    new ZLinkAggregateRelocationCoordinator.Participant(
                                            key,
                                            ZLinkPlacementObjectKind.USER_SPOT,
                                            current.objectGeneration(),
                                            current.authorityOwnerGeneration(),
                                            current.storeVersion(),
                                            transition,
                                            payload,
                                            new byte[0])),
                            root,
                            reservation.targetDescriptor(),
                            descriptor.lifecycleGeneration(),
                            ZLinkPlacementCapacityBundle.spot(
                                    ZLinkPlacementObjectKind.USER_SPOT, "room", 1),
                            owner,
                            current.storeVersion());
            var prepared =
                    new ZLinkAggregateRelocationCoordinator(repository)
                            .prepare(request, () -> false)
                            .toCompletableFuture()
                            .join();
            byte[] publishedPayload =
                    ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                            payload, request, transition);
            long targetGeneration =
                    current.authorityOwnerGeneration()
                            + (transition == ZLinkAuthorityGenerationTransition.NEW_OWNER ? 1 : 0);
            // Node encodeAuthorityRecord preserves this field order and uses decimal strings.
            var nodeMarker =
                    mapper.readTree(
                            """
                            {"aggregateId":"%s",
                             "aggregateGeneration":"1","index":0,"expectedStoreVersion":"%s",
                             "ownerTransition":"%s","targetAuthorityOwnerGeneration":"%s",
                             "authorityPayloadSha256":"%s","membershipMutationSha256":"%s"}
                            """
                                    .formatted(
                                            request.aggregateId(),
                                            current.storeVersion(),
                                            transition
                                                            == ZLinkAuthorityGenerationTransition
                                                                    .NEW_OWNER
                                                    ? "newOwner"
                                                    : "preserve",
                                            targetGeneration,
                                            HexFormat.of()
                                                    .formatHex(
                                                            ZLinkAggregateInventoryStore.sha256(
                                                                    publishedPayload)),
                                            HexFormat.of()
                                                    .formatHex(
                                                            ZLinkAggregateInventoryStore.sha256(
                                                                    new byte[0]))));
            var expected = (com.fasterxml.jackson.databind.node.ObjectNode) base;
            expected.set("aggregate", nodeMarker);
            expected.put("visibleStoreVersion", current.storeVersion());
            byte[] nodeBytes = mapper.writeValueAsBytes(expected);
            var marked =
                    assertInstanceOf(
                            ZLinkStoreReadFound.class,
                            provider.read(rowKey, () -> false).toCompletableFuture().join());
            assertArrayEquals(nodeBytes, marked.value().bytes());
            // Replacing the row with the independently assembled Node envelope exercises decode.
            provider.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(), List.of(new ZLinkStorePut(rowKey, nodeBytes, null))),
                            () -> false)
                    .toCompletableFuture()
                    .join();
            var pending =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(key, () -> false).toCompletableFuture().join());
            assertEquals(current.storeVersion(), pending.storeVersion());
            assertArrayEquals(payload, pending.payload());
            assertEquals(
                    ZLinkAggregateCommitResult.COMMITTED,
                    repository
                            .commitAggregate(prepared.fence(), () -> false)
                            .toCompletableFuture()
                            .join());
            var published =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            repository.read(key, () -> false).toCompletableFuture().join());
            assertArrayEquals(publishedPayload, published.payload());
            assertEquals(targetGeneration, published.authorityOwnerGeneration());
        }
    }

    @Test
    void reservationIdentityIsStableAcrossProviderConflict() throws Exception {
        var inner = new ZLinkInMemoryProviderLocationStore();
        var key = authorityKey(ZLinkAuthorityKeyCodec.spot("1304-stable-reservation"));
        var identities = new ArrayList<String>();
        ZLinkLocationStore provider =
                new ZLinkLocationStore() {
                    @Override
                    public CompletionStage<ZLinkStoreReadResult> read(
                            ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
                        return inner.read(key, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreWriteResult> write(
                            ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
                        for (var mutation : request.mutations()) {
                            if (mutation instanceof ZLinkStorePut put && put.key().equals(key)) {
                                try {
                                    identities.add(
                                            new com.fasterxml.jackson.databind.ObjectMapper()
                                                    .readTree(put.bytes())
                                                    .path("pendingCreation")
                                                    .path("reservationId")
                                                    .asText());
                                } catch (java.io.IOException failure) {
                                    return CompletableFuture.failedFuture(failure);
                                }
                                if (identities.size() == 1) {
                                    return CompletableFuture.completedFuture(
                                            new systems.zlink.framework.locationprovider
                                                    .ZLinkStoreWriteConflict(Instant.now()));
                                }
                            }
                        }
                        return inner.write(request, cancellation);
                    }

                    @Override
                    public CompletionStage<ZLinkStoreScanResult> scan(
                            ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
                        return inner.scan(request, cancellation);
                    }
                };
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("1304-stable-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var request =
                capacityRequest(
                        ZLinkAuthorityKeyCodec.spot("1304-stable-reservation"), descriptor, owner);
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(request, () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        assertEquals(2, identities.size());
        assertTrue(!identities.get(0).isEmpty());
        assertEquals(identities.get(0), identities.get(1));
        assertEquals(identities.get(0), reservation.reservationVersion());
    }

    @Test
    void creationCompletionRejectsExpiredOwnerWithoutConsumingReservedCapacity() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("1304-expired-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        descriptors
                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture()
                .get();
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var request =
                capacityRequest(
                        ZLinkAuthorityKeyCodec.spot("1304-expired-spot"), descriptor, owner);
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(request, () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        owners.release(owner).toCompletableFuture().get();
        assertEquals(
                ZLinkObjectCommitResult.STALE,
                repository
                        .commit(reservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        assertEquals(
                ZLinkObjectAbortResult.STALE,
                repository.abort(reservation, () -> false).toCompletableFuture().get());
        var authority =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository
                                .read(request.authorityKey(), () -> false)
                                .toCompletableFuture()
                                .get());
        assertEquals(reservation.storeVersion(), authority.storeVersion());
        assertTrue(authority.pendingCreation().isPresent());
    }

    @Test
    void opaqueProviderCapacityIsReservedCommittedAndReleasedAtomically() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("owner-capacity", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var first = capacityRequest(ZLinkAuthorityKeyCodec.spot("spot-a"), descriptor, owner);
        var second = capacityRequest(ZLinkAuthorityKeyCodec.spot("spot-b"), descriptor, owner);

        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository.reserve(first, () -> false).toCompletableFuture().get())
                        .reservation();
        assertCounterValue(provider, OBJECT_COUNTER_KEY, "2");
        assertCounterValue(provider, AUTHORITY_OWNER_COUNTER_KEY, "2");
        assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                repository.reserve(second, () -> false).toCompletableFuture().get());
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(reservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkPlacementCapacityExhausted.class,
                repository.reserve(second, () -> false).toCompletableFuture().get());

        var current =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository
                                .read(ZLinkAuthorityKeyCodec.spot("spot-a"), () -> false)
                                .toCompletableFuture()
                                .get());
        assertInstanceOf(
                ZLinkAuthorityDeleted.class,
                repository
                        .compareExchange(
                                ZLinkAuthorityKeyCodec.spot("spot-a"),
                                new ZLinkAuthorityExpectFound(current.storeVersion()),
                                new ZLinkAuthorityDelete(),
                                () -> false)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkObjectReserved.class,
                repository.reserve(second, () -> false).toCompletableFuture().get());
    }

    @Test
    void expiredOwnerLeaseWithMatchingGenerationReclaimsReservedAuthority() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var staleOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("expired-reservation-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var replacementOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("replacement-reservation-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var staleDescriptor =
                capacityDescriptor(staleOwner, "mesh", RoutingId.from("expired-reservation-node"));
        var replacementDescriptor =
                capacityDescriptor(
                        replacementOwner, "mesh", RoutingId.from("replacement-reservation-node"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(staleDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(replacementDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        String authorityKey = ZLinkAuthorityKeyCodec.actor("expired-reservation");
        var reserved =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                new ZLinkProviderAuthorityRepository(provider, descriptors)
                                        .reserve(
                                                capacityRequest(
                                                        authorityKey, staleDescriptor, staleOwner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();

        var replacement =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                new ZLinkProviderAuthorityRepository(
                                                new OwnerLeaseExpiryStore(
                                                        provider, staleOwner.ownerId(), true),
                                                descriptors)
                                        .reserve(
                                                capacityRequest(
                                                        authorityKey,
                                                        replacementDescriptor,
                                                        replacementOwner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();

        assertTrue(replacement.objectGeneration() > reserved.objectGeneration());
        assertEquals(replacementOwner, replacement.targetOwner());
    }

    @Test
    void liveOwnerLeaseWithMatchingGenerationProtectsReservedAuthority() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var liveOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("live-reservation-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var otherOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("other-reservation-owner", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var liveDescriptor =
                capacityDescriptor(liveOwner, "mesh", RoutingId.from("live-reservation-node"));
        var otherDescriptor =
                capacityDescriptor(otherOwner, "mesh", RoutingId.from("other-reservation-node"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(liveDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(otherDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        String authorityKey = ZLinkAuthorityKeyCodec.actor("live-reservation");
        assertInstanceOf(
                ZLinkObjectReserved.class,
                new ZLinkProviderAuthorityRepository(provider, descriptors)
                        .reserve(
                                capacityRequest(authorityKey, liveDescriptor, liveOwner),
                                () -> false)
                        .toCompletableFuture()
                        .get());

        var conflict =
                assertInstanceOf(
                        ZLinkObjectConflict.class,
                        new ZLinkProviderAuthorityRepository(
                                        new OwnerLeaseExpiryStore(
                                                provider, liveOwner.ownerId(), false),
                                        descriptors)
                                .reserve(
                                        capacityRequest(authorityKey, otherDescriptor, otherOwner),
                                        () -> false)
                                .toCompletableFuture()
                                .get());
        var existing = assertInstanceOf(ZLinkAuthoritySnapshot.class, conflict.current());

        assertEquals(liveOwner.ownerId(), existing.ownerId());
        assertEquals(liveOwner.leaseGeneration(), existing.ownerLeaseGeneration());
    }

    @Test
    void endedOwnerUnregisteredSpotTypeRetainsAuthority() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var oldOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-old", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var oldDescriptor = capacityDescriptor(oldOwner, "mesh", RoutingId.from("zone-rid-old"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(oldDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        String authorityKey = ZLinkAuthorityKeyCodec.spot("zone-nw");
        var oldReservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(
                                                        authorityKey, oldDescriptor, oldOwner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(oldReservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                owners.release(oldOwner).toCompletableFuture().get());

        var replacementOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-replacement", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var replacementDescriptor =
                capacityDescriptor(
                        replacementOwner, "mesh", RoutingId.from("zone-rid-replacement"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(replacementDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());

        assertUnavailableActiveAuthority(
                repository,
                authorityKey,
                oldReservation,
                () ->
                        repository
                                .reserve(
                                        capacityRequest(
                                                authorityKey,
                                                replacementDescriptor,
                                                replacementOwner),
                                        () -> false)
                                .toCompletableFuture()
                                .get());
    }

    @Test
    void endedOwnerUnregisteredSpotTypeRetainsAuthorityWithoutCapacity() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var oldOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-old", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var oldDescriptor = capacityDescriptor(oldOwner, "mesh", RoutingId.from("zone-rid-old"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(oldDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        String authorityKey = ZLinkAuthorityKeyCodec.actor("bot-se-y");
        var oldReservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(
                                                        authorityKey, oldDescriptor, oldOwner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(oldReservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                owners.release(oldOwner).toCompletableFuture().get());
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .removeMeshNode(
                                new ZLinkMeshNodeDescriptorKey(
                                        oldDescriptor.meshName(), oldDescriptor.rid()),
                                oldOwner)
                        .toCompletableFuture()
                        .get());
        provider.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStoreDelete(
                                                new ZLinkStoreKey(
                                                        "zlink:v11:capacity:mesh:zone-rid-old")))),
                        () -> false)
                .toCompletableFuture()
                .get();

        var replacementOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-replacement", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var replacementDescriptor =
                capacityDescriptor(
                        replacementOwner, "mesh", RoutingId.from("zone-rid-replacement"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(replacementDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());

        assertUnavailableActiveAuthority(
                repository,
                authorityKey,
                oldReservation,
                () ->
                        repository
                                .reserve(
                                        capacityRequest(
                                                authorityKey,
                                                replacementDescriptor,
                                                replacementOwner),
                                        () -> false)
                                .toCompletableFuture()
                                .get());
    }

    @Test
    void endedOwnerUnregisteredSpotTypeRetainsAuthorityWithEmptyCapacity() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        String authorityKey = ZLinkAuthorityKeyCodec.actor("bot-se-x");
        var oldOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-old", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var oldDescriptor = capacityDescriptor(oldOwner, "mesh", RoutingId.from("zone-rid-old"));
        var oldReservation =
                commitStaleAuthority(
                        repository, descriptors, oldDescriptor, oldOwner, authorityKey);
        // The dead owner's capacity row was recreated from zero by the
        // replacement, so it no longer accounts for the committed allocation.
        // Decrementing it would underflow, which used to abort the reclaim.
        zeroCapacityRow(provider, "zlink:v11:capacity:mesh:zone-rid-old");
        assertEquals(
                ZLinkOwnerLeaseReleaseResult.RELEASED,
                owners.release(oldOwner).toCompletableFuture().get());

        var replacementOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-replacement", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var replacementDescriptor =
                capacityDescriptor(
                        replacementOwner, "mesh", RoutingId.from("zone-rid-replacement"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(replacementDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());

        assertUnavailableActiveAuthority(
                repository,
                authorityKey,
                oldReservation,
                () ->
                        repository
                                .reserve(
                                        capacityRequest(
                                                authorityKey,
                                                replacementDescriptor,
                                                replacementOwner),
                                        () -> false)
                                .toCompletableFuture()
                                .get());
    }

    @Test
    void underflowingCapacityRowNeverReclaimsAuthorityFromALiveOwner() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        String authorityKey = ZLinkAuthorityKeyCodec.actor("bot-se-x");
        var liveOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-live", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var liveDescriptor = capacityDescriptor(liveOwner, "mesh", RoutingId.from("zone-rid-live"));
        commitStaleAuthority(repository, descriptors, liveDescriptor, liveOwner, authorityKey);
        zeroCapacityRow(provider, "zlink:v11:capacity:mesh:zone-rid-live");

        var otherOwner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("zone-node-other", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var otherDescriptor =
                capacityDescriptor(otherOwner, "mesh", RoutingId.from("zone-rid-other"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(otherDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());

        // The owner lease is still current, so the row is not an orphan and the
        // authority must stay exactly where it is.
        var existing =
                assertInstanceOf(
                                ZLinkObjectAlreadyExists.class,
                                repository
                                        .reserve(
                                                capacityRequest(
                                                        authorityKey, otherDescriptor, otherOwner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .current();
        assertEquals(liveDescriptor.rid(), existing.allocation().descriptor().rid());
    }

    @Test
    void capacityRowsUseTheNodeCppCanonicalKeyAndJsonShape() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                new ZLinkProviderOwnerLeaseRepository(provider)
                                        .claim("owner-canonical-capacity", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner, "mesh /한", RoutingId.from("node /#?"));
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());

        var repository = new ZLinkProviderAuthorityRepository(provider, descriptors);
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(
                                                        ZLinkAuthorityKeyCodec.spot(
                                                                "spot-canonical"),
                                                        descriptor,
                                                        owner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        String key = "zlink:v11:capacity:mesh%20%2F%ED%95%9C:node%20%2F%23%3F";
        assertCapacityRow(
                provider,
                key,
                "{\"active\":{\"actors\":0,\"spots\":0,\"spotTypes\":{}},"
                        + "\"pending\":{\"actors\":0,\"spots\":1,\"spotTypes\":{"
                        + "\"user_spot\\u0000room\":1}}}");

        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(reservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        assertCapacityRow(
                provider,
                key,
                "{\"active\":{\"actors\":0,\"spots\":1,\"spotTypes\":{"
                        + "\"user_spot\\u0000room\":1}},\"pending\":{\"actors\":0,"
                        + "\"spots\":0,\"spotTypes\":{}}}");
    }

    @Test
    void exhaustedObjectCounterReturnsTypedResultWithoutChangingIt() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owners = new ZLinkProviderOwnerLeaseRepository(provider);
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                owners.claim("owner-exhausted", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        var descriptors = new ZLinkProviderDescriptorRepository(provider);
        var descriptor = capacityDescriptor(owner);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        ZLinkStoreKey counterKey = new ZLinkStoreKey(OBJECT_COUNTER_KEY);
        provider.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                counterKey,
                                                Long.toString(Long.MAX_VALUE)
                                                        .getBytes(
                                                                java.nio.charset.StandardCharsets
                                                                        .UTF_8),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .get();

        var result =
                new ZLinkProviderAuthorityRepository(provider, descriptors)
                        .reserve(
                                capacityRequest(
                                        ZLinkAuthorityKeyCodec.spot("spot-exhausted"),
                                        descriptor,
                                        owner),
                                () -> false)
                        .toCompletableFuture()
                        .get();

        assertInstanceOf(ZLinkObjectGenerationExhausted.class, result);
        var counter =
                assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(counterKey, () -> false).toCompletableFuture().get());
        assertEquals(
                Long.toString(Long.MAX_VALUE),
                new String(counter.value().bytes(), java.nio.charset.StandardCharsets.UTF_8));
    }

    @Test
    void reserveRejectsNonCanonicalObjectAndOwnerCounterRecords() throws Exception {
        for (String counterName : List.of(OBJECT_COUNTER_KEY, AUTHORITY_OWNER_COUNTER_KEY)) {
            for (String invalid : List.of("01", "+1", "0", "", " 1")) {
                var provider = new ZLinkInMemoryProviderLocationStore();
                var owner =
                        ((ZLinkOwnerLeaseClaimed)
                                        new ZLinkProviderOwnerLeaseRepository(provider)
                                                .claim("owner-counter", Duration.ofMinutes(1))
                                                .toCompletableFuture()
                                                .get())
                                .token();
                var descriptors = new ZLinkProviderDescriptorRepository(provider);
                var descriptor = capacityDescriptor(owner);
                assertEquals(
                        ZLinkLocationWriteStatus.STORED,
                        descriptors
                                .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                                .toCompletableFuture()
                                .get()
                                .status());
                provider.write(
                                new ZLinkStoreWriteRequest(
                                        List.of(),
                                        List.of(
                                                new ZLinkStorePut(
                                                        new ZLinkStoreKey(counterName),
                                                        invalid.getBytes(
                                                                java.nio.charset.StandardCharsets
                                                                        .UTF_8),
                                                        null))),
                                () -> false)
                        .toCompletableFuture()
                        .get();

                var failure =
                        assertThrows(
                                java.util.concurrent.ExecutionException.class,
                                () ->
                                        new ZLinkProviderAuthorityRepository(provider, descriptors)
                                                .reserve(
                                                        capacityRequest(
                                                                ZLinkAuthorityKeyCodec.spot(
                                                                        "spot-counter"),
                                                                descriptor,
                                                                owner),
                                                        () -> false)
                                                .toCompletableFuture()
                                                .get());
                assertInstanceOf(IllegalStateException.class, failure.getCause());
                assertEquals("Location Store counter is invalid", failure.getCause().getMessage());
            }
        }
    }

    @Test
    void nearCeilingAggregateCounterBlockReturnsTypedExhaustion() throws Exception {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var owner =
                ((ZLinkOwnerLeaseClaimed)
                                new ZLinkProviderOwnerLeaseRepository(provider)
                                        .claim("owner-a", Duration.ofMinutes(1))
                                        .toCompletableFuture()
                                        .get())
                        .token();
        String authorityA = ZLinkAuthorityKeyCodec.actor("ceiling-a");
        String authorityB = ZLinkAuthorityKeyCodec.actor("ceiling-b");
        var keyA = authorityKey(authorityA);
        var keyB = authorityKey(authorityB);
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        provider.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                keyA,
                                                                encodedAuthorityRecord(),
                                                                null),
                                                        new ZLinkStorePut(
                                                                keyB,
                                                                encodedAuthorityRecord(),
                                                                null),
                                                        new ZLinkStorePut(
                                                                new ZLinkStoreKey(
                                                                        AUTHORITY_OWNER_COUNTER_KEY),
                                                                Long.toString(Long.MAX_VALUE - 1)
                                                                        .getBytes(
                                                                                java.nio.charset
                                                                                        .StandardCharsets
                                                                                        .UTF_8),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .get();
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 12),
                        1,
                        List.of(
                                new ZLinkAggregateParticipant(
                                        authorityA,
                                        1,
                                        1,
                                        seeded.putVersions().get(keyA).value(),
                                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                        new byte[] {1},
                                        new byte[] {2}),
                                new ZLinkAggregateParticipant(
                                        authorityB,
                                        1,
                                        1,
                                        seeded.putVersions().get(keyB).value(),
                                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                        new byte[] {3},
                                        new byte[] {4})),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1),
                        owner);

        var result =
                new ZLinkProviderAuthorityRepository(provider)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .get();

        assertInstanceOf(ZLinkAggregateGenerationExhausted.class, result);
        assertCounterValue(
                provider, AUTHORITY_OWNER_COUNTER_KEY, Long.toString(Long.MAX_VALUE - 1));
    }

    @Test
    void aggregateMarkerRoundTripPreservesCompletionCounters() throws ReflectiveOperationException {
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 9),
                        7,
                        List.of(participant("actor:a"), participant("spot:b")),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
                        4,
                        ZLinkPlacementCapacityBundle.actor(1),
                        new ZLinkLocationOwnerToken("owner-b", 12));
        Method encode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "encodeAggregate", byte.class, ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        byte[] encoded = (byte[]) encode.invoke(null, (byte) 2, request);

        Method decode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "decodeAggregate", byte[].class);
        decode.setAccessible(true);
        Object decoded = decode.invoke(null, encoded);
        Method decodedId = decoded.getClass().getDeclaredMethod("aggregateId");
        decodedId.setAccessible(true);

        assertEquals(request.aggregateId(), decodedId.invoke(decoded));
    }

    @Test
    void stagingMarkerStoresMetadataInsteadOfParticipantPayloads()
            throws ReflectiveOperationException {
        var participants =
                IntStream.range(0, 2050)
                        .mapToObj(index -> participant("authority-%04d".formatted(index)))
                        .toList();
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 10),
                        8,
                        participants,
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-b")),
                        4,
                        ZLinkPlacementCapacityBundle.actor(1),
                        new ZLinkLocationOwnerToken("owner-b", 12));

        Method encode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "encodeAggregate", byte.class, ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        byte[] encoded = (byte[]) encode.invoke(null, (byte) 0, request);

        assertTrue(encoded.length < 1024);

        Method decode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "decodeAggregate", byte[].class);
        decode.setAccessible(true);
        Object decoded = decode.invoke(null, encoded);
        Method state = decoded.getClass().getDeclaredMethod("state");
        Method participantCount = decoded.getClass().getDeclaredMethod("participantCount");
        state.setAccessible(true);
        participantCount.setAccessible(true);

        assertEquals((byte) 0, state.invoke(decoded));
        assertEquals(2050, participantCount.invoke(decoded));
    }

    @Test
    void ownerCleanupContinuesAfterTheFirstProviderScanPage() throws ReflectiveOperationException {
        var store = new ZLinkInMemoryProviderLocationStore();
        var encodedAuthority = encodedAuthorityRecord();
        var mutations =
                new ArrayList<systems.zlink.framework.locationprovider.ZLinkStoreMutation>();
        for (int index = 0; index < 1_001; index++) {
            String authorityKey = "authority\0actor\0authority-" + index;
            mutations.add(
                    new ZLinkStorePut(new ZLinkStoreKey(authorityKey), encodedAuthority, null));
        }
        store.write(new ZLinkStoreWriteRequest(List.of(), mutations), () -> false)
                .toCompletableFuture()
                .join();

        var repository = new ZLinkProviderAuthorityRepository(store);
        long removed =
                repository
                        .removeAllByOwner(new ZLinkLocationOwnerToken("owner-a", 1))
                        .toCompletableFuture()
                        .join();

        assertEquals(1_001L, removed);
        var remaining =
                (ZLinkStoreScanPageResult)
                        store.scan(
                                        new ZLinkStoreScanRequest("authority\0", null, 1_000),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        assertTrue(remaining.value().items().isEmpty());
    }

    @Test
    void rollbackDoesNotRemoveAnExistingParticipantMarker() throws ReflectiveOperationException {
        String authorityAContractKey = ZLinkAuthorityKeyCodec.actor("authority-a");
        String authorityBContractKey = ZLinkAuthorityKeyCodec.actor("authority-b");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var store = new FailingParticipantStore(delegate, authorityBContractKey);
        var ownerLeases = new ZLinkProviderOwnerLeaseRepository(store);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        ownerLeases
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        var authorityA = authorityKey(authorityAContractKey);
        var authorityB = authorityKey(authorityBContractKey);
        var seed = encodedAuthorityRecord();
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
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
        var participantA =
                participant(authorityAContractKey, versionA, new byte[] {11}, new byte[] {12});
        var participantB =
                participant(authorityBContractKey, versionB, new byte[] {21}, new byte[] {22});
        var request =
                new ZLinkAggregatePrepareRequest(
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
                                List.of(
                                        new ZLinkStorePut(
                                                authorityA, encodedAuthorityRecord(marker), null))),
                        () -> false)
                .toCompletableFuture()
                .join();
        delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                aggregateKey(request),
                                                encodedAggregateStaging(request),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();

        var repository = new ZLinkProviderAuthorityRepository(store);
        var result = repository.prepareAggregate(request, () -> false).toCompletableFuture().join();

        assertTrue(
                store.failingWrites > 0,
                () -> "participant write was not intercepted; result=" + result);
        assertEquals(
                ZLinkAggregateConflict.class,
                result.getClass(),
                () -> "unexpected prepare result: " + result);
        var decoded =
                decodeAuthority(
                        ((ZLinkStoreReadFound)
                                        delegate.read(authorityA, () -> false)
                                                .toCompletableFuture()
                                                .join())
                                .value()
                                .bytes());
        Method aggregate = decoded.getClass().getDeclaredMethod("aggregate");
        aggregate.setAccessible(true);
        assertNotNull(aggregate.invoke(decoded));
    }

    @Test
    void aggregatePrepareAdoptsPeerMarkerAfterAuthorityCounterCasConflict()
            throws ReflectiveOperationException {
        String authorityContractKey = ZLinkAuthorityKeyCodec.actor("authority-counter-race");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var store = new CounterContentionStore(delegate);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        ZLinkStoreKey authority = authorityKey(authorityContractKey);
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authority,
                                                                encodedAuthorityRecord(),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        var participant =
                new ZLinkAggregateParticipant(
                        authorityContractKey,
                        1,
                        1,
                        seeded.putVersions().get(authority).value(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        new byte[] {11},
                        new byte[] {12});
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 12),
                        1,
                        List.of(participant),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1),
                        owner.token());

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .join();

        assertTrue(
                store.publishedPeerMarker, "test store did not publish the contended marker write");
        assertTrue(
                result instanceof ZLinkAggregatePrepared
                        || result instanceof ZLinkAggregateAlreadyPrepared,
                () -> "marker CAS contention did not converge: " + result);
        var aggregate =
                (ZLinkStoreReadFound)
                        delegate.read(aggregateKey(request), () -> false)
                                .toCompletableFuture()
                                .join();
        assertEquals((byte) 1, stateOf(aggregate.value().bytes()));
    }

    @Test
    void aggregatePrepareReentersAfterIndependentAuthorityCounterContention()
            throws ReflectiveOperationException {
        String authorityContractKey = ZLinkAuthorityKeyCodec.actor("authority-counter-only-race");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        ZLinkStoreKey authority = authorityKey(authorityContractKey);
        var store = new IndependentCounterContentionStore(delegate, authority, "2", null);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authority,
                                                                encodedAuthorityRecord(),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        var participant =
                new ZLinkAggregateParticipant(
                        authorityContractKey,
                        1,
                        1,
                        seeded.putVersions().get(authority).value(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        new byte[] {11},
                        new byte[] {12});
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 13),
                        1,
                        List.of(participant),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1),
                        owner.token());

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .join();

        assertTrue(store.bumpedCounter, "test store did not create counter contention");
        assertTrue(
                result instanceof ZLinkAggregatePrepared
                        || result instanceof ZLinkAggregateAlreadyPrepared,
                () -> "unchanged aggregate did not re-enter prepare: " + result);
        var aggregate =
                (ZLinkStoreReadFound)
                        delegate.read(aggregateKey(request), () -> false)
                                .toCompletableFuture()
                                .join();
        assertEquals((byte) 1, stateOf(aggregate.value().bytes()));
    }

    @Test
    void aggregatePreparePropagatesCounterExhaustionFromReentry()
            throws ReflectiveOperationException {
        String authorityContractKey = ZLinkAuthorityKeyCodec.actor("authority-counter-exhausted");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        ZLinkStoreKey authority = authorityKey(authorityContractKey);
        var store =
                new IndependentCounterContentionStore(
                        delegate, authority, Long.toString(Long.MAX_VALUE), null);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authority,
                                                                encodedAuthorityRecord(),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        var participant =
                new ZLinkAggregateParticipant(
                        authorityContractKey,
                        1,
                        1,
                        seeded.putVersions().get(authority).value(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        new byte[] {11},
                        new byte[] {12});
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 15),
                        1,
                        List.of(participant),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1),
                        owner.token());

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .join();

        assertTrue(store.bumpedCounter, "test store did not exhaust the contended counter");
        assertInstanceOf(ZLinkAggregateGenerationExhausted.class, result);
    }

    @Test
    void adoptedAggregatePrepareClearsInstalledMarkersBeforePropagatingReentryTerminal()
            throws ReflectiveOperationException {
        String authorityAContractKey = ZLinkAuthorityKeyCodec.actor("authority-adopted-a");
        String authorityBContractKey = ZLinkAuthorityKeyCodec.actor("authority-adopted-b");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        ZLinkStoreKey authorityA = authorityKey(authorityAContractKey);
        ZLinkStoreKey authorityB = authorityKey(authorityBContractKey);
        var store =
                new IndependentCounterContentionStore(
                        delegate, authorityB, Long.toString(Long.MAX_VALUE), null);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authorityA,
                                                                encodedAuthorityRecord(),
                                                                null),
                                                        new ZLinkStorePut(
                                                                authorityB,
                                                                encodedAuthorityRecord(),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        var participantA =
                participant(
                        authorityAContractKey,
                        seeded.putVersions().get(authorityA).value(),
                        new byte[] {11},
                        new byte[] {12});
        var participantB =
                new ZLinkAggregateParticipant(
                        authorityBContractKey,
                        1,
                        1,
                        seeded.putVersions().get(authorityB).value(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        new byte[] {21},
                        new byte[] {22});
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 16),
                        1,
                        List.of(participantA, participantB),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(2),
                        owner.token());
        delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                aggregateKey(request),
                                                encodedAggregateStaging(request),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .join();

        assertInstanceOf(ZLinkAggregateGenerationExhausted.class, result);
        var currentA =
                (ZLinkStoreReadFound)
                        delegate.read(authorityA, () -> false).toCompletableFuture().join();
        Object decodedA = decodeAuthority(currentA.value().bytes());
        Method aggregate = decodedA.getClass().getDeclaredMethod("aggregate");
        aggregate.setAccessible(true);
        assertNull(aggregate.invoke(decodedA), "prepare left an installed participant marker");
    }

    @Test
    void aggregatePrepareStillConflictsWhenParticipantFenceChangesDuringCounterContention()
            throws ReflectiveOperationException {
        String authorityContractKey = ZLinkAuthorityKeyCodec.actor("authority-fence-race");
        var delegate = new ZLinkInMemoryProviderLocationStore();
        ZLinkStoreKey authority = authorityKey(authorityContractKey);
        var store =
                new IndependentCounterContentionStore(
                        delegate, authority, "2", encodedAuthorityRecord());
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        var seeded =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authority,
                                                                encodedAuthorityRecord(),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        var participant =
                new ZLinkAggregateParticipant(
                        authorityContractKey,
                        1,
                        1,
                        seeded.putVersions().get(authority).value(),
                        ZLinkAuthorityGenerationTransition.NEW_OWNER,
                        new byte[] {11},
                        new byte[] {12});
        var request =
                new ZLinkAggregatePrepareRequest(
                        new UUID(0, 14),
                        1,
                        List.of(participant),
                        new byte[32],
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1),
                        owner.token());

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .prepareAggregate(request, () -> false)
                        .toCompletableFuture()
                        .join();

        assertTrue(store.bumpedCounter, "test store did not create counter contention");
        assertInstanceOf(ZLinkAggregateConflict.class, result);
    }

    @Test
    void aggregateCommitRetriesAConditionalWriteConflict() throws ReflectiveOperationException {
        var delegate = new ZLinkInMemoryProviderLocationStore();
        var store = new FailingParticipantStore(delegate, null, true);
        var owner =
                (ZLinkOwnerLeaseClaimed)
                        new ZLinkProviderOwnerLeaseRepository(store)
                                .claim("owner-a", Duration.ofHours(1))
                                .toCompletableFuture()
                                .join();
        String authorityAContractKey = ZLinkAuthorityKeyCodec.actor("authority-a");
        var authorityKey = authorityKey(authorityAContractKey);
        var basePayload =
                new ZLinkActorAuthorityPayloadCodec()
                        .encode(
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
        var seed =
                (systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied)
                        delegate.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                authorityKey,
                                                                encodedAuthorityRecord(
                                                                        null, basePayload),
                                                                null))),
                                        () -> false)
                                .toCompletableFuture()
                                .join();
        String version = seed.putVersions().get(authorityKey).value();
        var relocationRequest =
                new ZLinkAggregateRelocationCoordinator.Request(
                        new UUID(0, 9),
                        1,
                        2,
                        List.of(
                                new ZLinkAggregateRelocationCoordinator.Participant(
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
        var participant =
                new ZLinkAggregateParticipant(
                        authorityAContractKey,
                        1,
                        1,
                        version,
                        ZLinkAuthorityGenerationTransition.PRESERVE,
                        canonicalPayload,
                        new byte[0]);
        var request =
                new ZLinkAggregatePrepareRequest(
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
                                List.of(
                                        new ZLinkStorePut(
                                                authorityKey,
                                                encodedAuthorityRecord(marker, basePayload),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();
        new ZLinkAggregateInventoryStore(store)
                .store(request, () -> false)
                .toCompletableFuture()
                .join();
        delegate.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                aggregateKey(request),
                                                encodedAggregate((byte) 1, request),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .join();

        var result =
                new ZLinkProviderAuthorityRepository(store)
                        .commitAggregate(
                                new ZLinkAggregateFence(
                                        request.aggregateId(), request.aggregateGeneration()),
                                () -> false)
                        .toCompletableFuture()
                        .join();

        assertEquals(ZLinkAggregateCommitResult.COMMITTED, result);
        var committed =
                (systems.zlink.framework.locationprovider.ZLinkStoreReadFound)
                        delegate.read(aggregateKey(request), () -> false)
                                .toCompletableFuture()
                                .join();
        Method state =
                decodeAggregate(committed.value().bytes()).getClass().getDeclaredMethod("state");
        state.setAccessible(true);
        assertEquals((byte) 2, state.invoke(decodeAggregate(committed.value().bytes())));
    }

    @Test
    void aggregateCommitDoesNotBecomeStaleAfterMoreThanSixtyFourTemporaryConflicts() {
        assertAggregateCommitSurvivesTemporaryConflicts(65, Duration.ZERO);
    }

    @Test
    void aggregateCommitDoesNotBecomeStaleAfterFiveSecondsOfProviderLatency() {
        assertAggregateCommitSurvivesTemporaryConflicts(1, Duration.ofMillis(5100));
    }

    private static void assertAggregateCommitSurvivesTemporaryConflicts(
            int conflictCount, Duration providerLatency) {
        var store = new TemporaryCommitConflictStore(new ZLinkInMemoryProviderLocationStore());
        var repository = new ZLinkProviderLocationRepository(store);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                repository
                                        .claimOwnerLease("commit-owner", Duration.ofHours(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var descriptor = capacityDescriptor(owner);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                repository
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .join()
                        .status());
        String key = ZLinkAuthorityKeyCodec.spot("temporary-conflict-spot");
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(key, descriptor, owner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .join())
                        .reservation();
        byte[] payload =
                new systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec()
                        .encodeUser(
                                systems.zlink.framework.runtime.locations
                                        .ZLinkServiceAuthorityPayloadCodec.State.READY,
                                "room",
                                "temporary-conflict-spot",
                                owner.ownerId(),
                                owner.leaseGeneration(),
                                descriptor.meshName(),
                                descriptor.rid(),
                                descriptor.lifecycleGeneration());
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(reservation, payload, null, () -> false)
                        .toCompletableFuture()
                        .join());
        var current =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository.read(key, () -> false).toCompletableFuture().join());
        var request =
                new ZLinkAggregateRelocationCoordinator.Request(
                        new UUID(0, 9),
                        1,
                        1,
                        List.of(
                                new ZLinkAggregateRelocationCoordinator.Participant(
                                        key,
                                        ZLinkPlacementObjectKind.USER_SPOT,
                                        current.objectGeneration(),
                                        current.authorityOwnerGeneration(),
                                        current.storeVersion(),
                                        ZLinkAuthorityGenerationTransition.PRESERVE,
                                        payload,
                                        new byte[0])),
                        goldenRoot(),
                        reservation.targetDescriptor(),
                        descriptor.lifecycleGeneration(),
                        ZLinkPlacementCapacityBundle.spot(
                                ZLinkPlacementObjectKind.USER_SPOT, "room", 1),
                        owner,
                        current.storeVersion());
        var coordinator = new ZLinkAggregateRelocationCoordinator(repository);
        var prepared = coordinator.prepare(request, () -> false).toCompletableFuture().join();
        store.remainingConflicts = conflictCount;
        store.providerLatency = providerLatency;
        assertEquals(
                ZLinkAggregateCommitResult.COMMITTED,
                repository
                        .commitAggregate(prepared.fence(), () -> false)
                        .toCompletableFuture()
                        .join());
        assertEquals(conflictCount, store.conflictsReturned);
    }

    private static final class TemporaryCommitConflictStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private int remainingConflicts;
        private int conflictsReturned;
        private Duration providerLatency = Duration.ZERO;
        private java.util.function.Function<ZLinkStoreKey, CompletionStage<?>> beforeRead;

        private TemporaryCommitConflictStore(ZLinkLocationStore delegate) {
            this.delegate = delegate;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            var callback = beforeRead;
            return callback == null
                    ? delegate.read(key, cancellation)
                    : callback.apply(key).thenCompose(ignored -> delegate.read(key, cancellation));
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            if (remainingConflicts == 0) {
                return delegate.write(request, cancellation);
            }
            remainingConflicts--;
            conflictsReturned++;
            if (providerLatency.isZero()) {
                return CompletableFuture.completedFuture(
                        new systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict(
                                Instant.now()));
            }
            var result = new CompletableFuture<ZLinkStoreWriteResult>();
            CompletableFuture.delayedExecutor(
                            providerLatency.toMillis(), java.util.concurrent.TimeUnit.MILLISECONDS)
                    .execute(
                            () ->
                                    result.complete(
                                            new systems.zlink.framework.locationprovider
                                                    .ZLinkStoreWriteConflict(Instant.now())));
            return result;
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static byte[] encodedAuthorityRecord() throws ReflectiveOperationException {
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
                new ZLinkMeshNodeDescriptorKey(descriptor.meshName(), descriptor.rid()),
                descriptor.lifecycleGeneration(),
                owner,
                new byte[] {1},
                ZLinkPlacementCapacityBundle.spot(ZLinkPlacementObjectKind.USER_SPOT, "room", 1));
    }

    private static void assertCounterValue(ZLinkLocationStore provider, String key, String expected)
            throws Exception {
        var found =
                assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(new ZLinkStoreKey(key), () -> false)
                                .toCompletableFuture()
                                .get());
        assertEquals(
                expected,
                new String(found.value().bytes(), java.nio.charset.StandardCharsets.UTF_8));
    }

    private static void assertCapacityRow(ZLinkLocationStore provider, String key, String expected)
            throws Exception {
        var found =
                assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        provider.read(new ZLinkStoreKey(key), () -> false)
                                .toCompletableFuture()
                                .get());
        assertEquals(
                expected,
                new String(found.value().bytes(), java.nio.charset.StandardCharsets.UTF_8));
    }

    private static ZLinkObjectReservation commitStaleAuthority(
            ZLinkProviderAuthorityRepository repository,
            ZLinkProviderDescriptorRepository descriptors,
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationOwnerToken owner,
            String authorityKey)
            throws Exception {
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                descriptors
                        .updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .get()
                        .status());
        var reservation =
                assertInstanceOf(
                                ZLinkObjectReserved.class,
                                repository
                                        .reserve(
                                                capacityRequest(authorityKey, descriptor, owner),
                                                () -> false)
                                        .toCompletableFuture()
                                        .get())
                        .reservation();
        assertEquals(
                ZLinkObjectCommitResult.COMMITTED,
                repository
                        .commit(reservation, new byte[] {2}, null, () -> false)
                        .toCompletableFuture()
                        .get());
        return reservation;
    }

    private static void assertUnavailableActiveAuthority(
            ZLinkProviderAuthorityRepository repository,
            String authorityKey,
            ZLinkObjectReservation original,
            java.util.concurrent.Callable<ZLinkObjectReserveResult> result)
            throws Exception {
        // Location runtime §6.1: an ended owner with an unregistered policy is Unavailable.
        var failure = assertThrows(java.util.concurrent.ExecutionException.class, result::call);
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.UNAVAILABLE,
                assertInstanceOf(
                                systems.zlink.framework.errors.ZLinkFrameworkException.class,
                                failure.getCause())
                        .kind());
        var observed =
                assertInstanceOf(
                        ZLinkAuthoritySnapshot.class,
                        repository.read(authorityKey, () -> false).toCompletableFuture().get());
        assertEquals(original.objectGeneration(), observed.objectGeneration());
        assertEquals(original.targetOwner().ownerId(), observed.ownerId());
        assertEquals(original.targetOwner().leaseGeneration(), observed.ownerLeaseGeneration());
        assertEquals(original.targetDescriptor(), observed.allocation().descriptor());
        assertArrayEquals(new byte[] {2}, observed.payload());
    }

    private static void zeroCapacityRow(ZLinkLocationStore provider, String key) throws Exception {
        provider.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                new ZLinkStoreKey(key),
                                                ("{\"active\":{\"actors\":0,\"spots\":0,"
                                                                + "\"spotTypes\":{}},\"pending\":{\"actors\":0,"
                                                                + "\"spots\":0,\"spotTypes\":{}}}")
                                                        .getBytes(
                                                                java.nio.charset.StandardCharsets
                                                                        .UTF_8),
                                                null))),
                        () -> false)
                .toCompletableFuture()
                .get();
    }

    private static ZLinkMeshNodeDescriptor capacityDescriptor(ZLinkLocationOwnerToken owner) {
        return capacityDescriptor(owner, "game", RoutingId.from("capacity-node"));
    }

    private static ZLinkMeshNodeDescriptor capacityDescriptor(
            ZLinkLocationOwnerToken owner, String meshName, RoutingId nodeRid) {
        return new ZLinkMeshNodeDescriptor(
                meshName,
                nodeRid,
                1,
                1,
                "tcp://127.0.0.1:7100",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                "room",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                1)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("capacity-node-entry-00000000-0000-4000-8000-000000000001"),
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

    private static byte[] encodedAuthorityRecord(Object aggregate, byte[] payload)
            throws ReflectiveOperationException {
        Class<?> authorityRecord =
                Arrays.stream(ZLinkProviderAuthorityRepository.class.getDeclaredClasses())
                        .filter(type -> type.getSimpleName().equals("AuthorityRecord"))
                        .findFirst()
                        .orElseThrow();
        Constructor<?> constructor =
                Arrays.stream(authorityRecord.getDeclaredConstructors())
                        .filter(value -> value.getParameterCount() == 9)
                        .findFirst()
                        .orElseThrow();
        constructor.setAccessible(true);
        var allocation =
                new ZLinkPlacementAllocation(
                        ZLinkPlacementAllocationState.ACTIVE,
                        ZLinkPlacementObjectKind.ACTOR,
                        "actor",
                        new ZLinkMeshNodeDescriptorKey("game", RoutingId.from("node-a")),
                        1,
                        ZLinkPlacementCapacityBundle.actor(1));
        Object record =
                constructor.newInstance(
                        payload,
                        1L,
                        1L,
                        "owner-a",
                        1L,
                        allocation,
                        Optional.empty(),
                        aggregate,
                        null);
        Method encode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod("encode", authorityRecord);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(null, record);
    }

    private static ZLinkAggregateParticipant participant(String key) {
        return participant(key, "version-1", new byte[] {1}, new byte[] {2});
    }

    private static ZLinkAggregateParticipant participant(
            String key, String version, byte[] authorityPayload, byte[] membershipMutation) {
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
            ZLinkAggregatePrepareRequest request, ZLinkAggregateParticipant participant, int index)
            throws ReflectiveOperationException {
        Class<?> marker =
                Arrays.stream(ZLinkProviderAuthorityRepository.class.getDeclaredClasses())
                        .filter(type -> type.getSimpleName().equals("AggregateParticipantMarker"))
                        .findFirst()
                        .orElseThrow();
        Constructor<?> constructor = marker.getDeclaredConstructors()[0];
        constructor.setAccessible(true);
        Method sha256 =
                ZLinkAggregateInventoryStore.class.getDeclaredMethod("sha256", byte[].class);
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

    private static byte[] encodedAggregateStaging(ZLinkAggregatePrepareRequest request)
            throws ReflectiveOperationException {
        return encodedAggregate((byte) 0, request);
    }

    private static byte[] encodedAggregate(byte state, ZLinkAggregatePrepareRequest request)
            throws ReflectiveOperationException {
        Method encode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "encodeAggregate", byte.class, ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(null, state, request);
    }

    private static byte[] encodedCommittedAggregate(ZLinkAggregatePrepareRequest request)
            throws ReflectiveOperationException {
        Method encode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "encodeAggregate", byte.class, ZLinkAggregatePrepareRequest.class);
        encode.setAccessible(true);
        return (byte[]) encode.invoke(null, (byte) 2, request);
    }

    private static Object decodeAuthority(byte[] bytes) throws ReflectiveOperationException {
        Method decode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod("decode", byte[].class);
        decode.setAccessible(true);
        return decode.invoke(null, bytes);
    }

    private static Object decodeAggregate(byte[] bytes) throws ReflectiveOperationException {
        Method decode =
                ZLinkProviderAuthorityRepository.class.getDeclaredMethod(
                        "decodeAggregate", byte[].class);
        decode.setAccessible(true);
        return decode.invoke(null, bytes);
    }

    private static byte stateOf(byte[] bytes) throws ReflectiveOperationException {
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
        return new ZLinkStoreKey("authority\0" + identity.kind() + "\0" + identity.id());
    }

    private static ZLinkStoreKey aggregateKey(ZLinkAggregatePrepareRequest request) {
        return new ZLinkStoreKey(
                "zlink:v11:aggregate:"
                        + request.aggregateId()
                        + ":"
                        + request.aggregateGeneration());
    }

    private static byte[] goldenRoot() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        Pattern logicalHex = Pattern.compile("\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"");
        while (current != null) {
            Path fixture = current.resolve("runtime/protocol/golden/relocation-envelope-v1.json");
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
        throw new IllegalStateException("shared relocation fixture was not found");
    }

    private static final class OwnerLeaseExpiryStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final ZLinkStoreKey ownerKey;
        private final boolean expired;

        private OwnerLeaseExpiryStore(
                ZLinkLocationStore delegate, String ownerId, boolean expired) {
            this.delegate = delegate;
            this.ownerKey = ZLinkOwnerLeaseRecordCodec.key(ownerId);
            this.expired = expired;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation)
                    .thenApply(
                            read -> {
                                if (!key.equals(ownerKey)
                                        || !(read instanceof ZLinkStoreReadFound found)) {
                                    return read;
                                }
                                var value = found.value();
                                return new ZLinkStoreReadFound(
                                        new ZLinkStoreValue(
                                                value.bytes(),
                                                value.version(),
                                                expired
                                                        ? value.storeNow()
                                                        : value.storeNow()
                                                                .plus(Duration.ofMinutes(1)),
                                                value.storeNow()));
                            });
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static final class FailingParticipantStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private final ZLinkStoreKey failingKey;
        private boolean failCommitOnce;
        private int failingWrites;

        private FailingParticipantStore(ZLinkLocationStore delegate, String failingAuthority) {
            this(delegate, failingAuthority, false);
        }

        private FailingParticipantStore(
                ZLinkLocationStore delegate, String failingAuthority, boolean failCommitOnce) {
            this.delegate = delegate;
            this.failingKey = failingAuthority == null ? null : authorityKey(failingAuthority);
            this.failCommitOnce = failCommitOnce;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean writesFailingParticipant =
                    failingKey != null
                            && request.mutations().stream()
                                    .anyMatch(
                                            mutation ->
                                                    mutation instanceof ZLinkStorePut put
                                                            && put.key().equals(failingKey));
            boolean writesCommittedAggregate =
                    failCommitOnce
                            && request.mutations().stream()
                                    .anyMatch(
                                            mutation ->
                                                    mutation instanceof ZLinkStorePut put
                                                            && put.bytes().length > 0
                                                            && put.key()
                                                                    .value()
                                                                    .startsWith(
                                                                            "zlink:v11:aggregate:")
                                                            && put.bytes()[0] == 2);
            if (writesFailingParticipant || writesCommittedAggregate) {
                if (writesFailingParticipant) {
                    failingWrites++;
                }
                failCommitOnce = false;
                return CompletableFuture.completedFuture(
                        new systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict(
                                Instant.now()));
            }
            return delegate.write(request, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static final class CounterContentionStore implements ZLinkLocationStore {
        private final ZLinkLocationStore delegate;
        private boolean publishedPeerMarker;

        private CounterContentionStore(ZLinkLocationStore delegate) {
            this.delegate = delegate;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean reservesAuthorityOwnerGeneration =
                    !publishedPeerMarker
                            && request.mutations().stream()
                                    .anyMatch(
                                            mutation ->
                                                    mutation instanceof ZLinkStorePut put
                                                            && put.key()
                                                                    .value()
                                                                    .equals(
                                                                            AUTHORITY_OWNER_COUNTER_KEY));
            if (!reservesAuthorityOwnerGeneration) {
                return delegate.write(request, cancellation);
            }
            publishedPeerMarker = true;
            return delegate.write(request, cancellation)
                    .thenApply(
                            ignored ->
                                    new systems.zlink.framework.locationprovider
                                            .ZLinkStoreWriteConflict(Instant.now()));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }

    private static final class IndependentCounterContentionStore implements ZLinkLocationStore {
        private final ZLinkStoreKey counterKey;

        private final ZLinkLocationStore delegate;
        private final ZLinkStoreKey authorityKey;
        private final String counterValue;
        private final byte[] changedParticipant;
        private boolean bumpedCounter;

        private IndependentCounterContentionStore(
                ZLinkLocationStore delegate,
                ZLinkStoreKey authorityKey,
                String counterValue,
                byte[] changedParticipant) {
            this(
                    delegate,
                    authorityKey,
                    counterValue,
                    changedParticipant,
                    new ZLinkStoreKey(AUTHORITY_OWNER_COUNTER_KEY));
        }

        private IndependentCounterContentionStore(
                ZLinkLocationStore delegate,
                ZLinkStoreKey authorityKey,
                String counterValue,
                byte[] changedParticipant,
                ZLinkStoreKey counterKey) {
            this.counterKey = counterKey;
            this.delegate = delegate;
            this.authorityKey = authorityKey;
            this.counterValue = counterValue;
            this.changedParticipant = changedParticipant;
        }

        @Override
        public CompletionStage<ZLinkStoreReadResult> read(
                ZLinkStoreKey key, ZLinkStoreCancellation cancellation) {
            return delegate.read(key, cancellation);
        }

        @Override
        public CompletionStage<ZLinkStoreWriteResult> write(
                ZLinkStoreWriteRequest request, ZLinkStoreCancellation cancellation) {
            boolean writesCounterAndParticipant =
                    !bumpedCounter
                            && request.mutations().stream()
                                    .filter(ZLinkStorePut.class::isInstance)
                                    .map(ZLinkStorePut.class::cast)
                                    .map(ZLinkStorePut::key)
                                    .anyMatch(counterKey::equals)
                            && request.mutations().stream()
                                    .filter(ZLinkStorePut.class::isInstance)
                                    .map(ZLinkStorePut.class::cast)
                                    .map(ZLinkStorePut::key)
                                    .anyMatch(authorityKey::equals);
            if (!writesCounterAndParticipant) {
                return delegate.write(request, cancellation);
            }
            bumpedCounter = true;
            var mutations =
                    new ArrayList<systems.zlink.framework.locationprovider.ZLinkStoreMutation>();
            mutations.add(
                    new ZLinkStorePut(
                            counterKey,
                            counterValue.getBytes(java.nio.charset.StandardCharsets.UTF_8),
                            null));
            if (changedParticipant != null) {
                mutations.add(new ZLinkStorePut(authorityKey, changedParticipant, null));
            }
            return delegate.write(new ZLinkStoreWriteRequest(List.of(), mutations), cancellation)
                    .thenCompose(ignored -> delegate.write(request, cancellation));
        }

        @Override
        public CompletionStage<ZLinkStoreScanResult> scan(
                ZLinkStoreScanRequest request, ZLinkStoreCancellation cancellation) {
            return delegate.scan(request, cancellation);
        }
    }
}
