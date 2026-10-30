package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;

import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;

final class ZLinkProviderDescriptorCapacityProjectionTest {
    @Test
    void providerCountsFollowReservationCommitAndRelease() {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var writer = new ZLinkProviderLocationRepository(provider);
        var reader = new ZLinkProviderLocationRepository(provider);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                writer.claimOwnerLease("usage-owner", Duration.ofHours(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        var descriptor = descriptor(owner, "usage-a", 0);
        assertEquals(
                ZLinkLocationWriteStatus.STORED,
                writer.updateMeshNode(descriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
                        .toCompletableFuture()
                        .join()
                        .status());
        var actor =
                reserve(
                        writer,
                        descriptor,
                        owner,
                        ZLinkPlacementObjectKind.ACTOR,
                        "player",
                        "usage-actor");
        var user =
                reserve(
                        writer,
                        descriptor,
                        owner,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "room",
                        "usage-user");
        var instance =
                reserve(
                        writer,
                        descriptor,
                        owner,
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        "dungeon",
                        "usage-instance");
        assertCapacity(only(reader).capacity(), 0, 1, 0, 2, 0, 1, 0, 1);
        for (var reservation : List.of(actor, user, instance)) {
            assertEquals(
                    ZLinkObjectCommitResult.COMMITTED,
                    writer.commit(reservation, new byte[] {2}, null, () -> false)
                            .toCompletableFuture()
                            .join());
        }
        var active = only(reader);
        assertCapacity(active.capacity(), 1, 0, 2, 0, 1, 0, 1, 0);
        assertEquals(descriptor.rid(), active.rid());
        assertEquals(descriptor.lifecycleGeneration(), active.lifecycleGeneration());
        assertEquals(descriptor.descriptorRevision(), active.descriptorRevision());
        assertEquals(descriptor.ownerId(), active.ownerId());
        assertEquals(descriptor.leaseGeneration(), active.leaseGeneration());
        assertEquals(descriptor.endpoint(), active.endpoint());
        assertEquals(descriptor.updatedAt(), active.updatedAt());
        for (var reservation : List.of(actor, user, instance)) {
            var current =
                    assertInstanceOf(
                            ZLinkAuthoritySnapshot.class,
                            writer.read(reservation.authorityKey(), () -> false)
                                    .toCompletableFuture()
                                    .join());
            assertInstanceOf(
                    ZLinkAuthorityDeleted.class,
                    writer.compareExchange(
                                    reservation.authorityKey(),
                                    new ZLinkAuthorityExpectFound(current.storeVersion()),
                                    new ZLinkAuthorityDelete(),
                                    () -> false)
                            .toCompletableFuture()
                            .join());
        }
        assertCapacity(only(reader).capacity(), 0, 0, 0, 0, 0, 0, 0, 0);
        var pending =
                reserve(
                        writer,
                        descriptor,
                        owner,
                        ZLinkPlacementObjectKind.USER_SPOT,
                        "room",
                        "usage-aborted");
        assertCapacity(only(reader).capacity(), 0, 0, 0, 1, 0, 1, 0, 0);
        writer.abort(pending, () -> false).toCompletableFuture().join();
        assertCapacity(only(reader).capacity(), 0, 0, 0, 0, 0, 0, 0, 0);
    }

    @Test
    void missingCapacityZeroesUsageAndPreservesDescriptorPagesAndLimits() {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var writer = new ZLinkProviderLocationRepository(provider);
        var reader = new ZLinkProviderLocationRepository(provider);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                writer.claimOwnerLease("missing-usage-owner", Duration.ofHours(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        for (var rid : List.of("usage-a", "usage-b")) {
            writer.updateMeshNode(descriptor(owner, rid, 1), ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .join();
        }
        var first =
                reader.listMeshNodes("usage", new ZLinkPageRequest(1, null))
                        .toCompletableFuture()
                        .join();
        assertEquals(1, first.items().size());
        assertNotNull(first.continuationToken());
        assertCapacity(first.items().getFirst().capacity(), 0, 0, 0, 0, 0, 0, 0, 0);
        var second =
                reader.listMeshNodes("usage", new ZLinkPageRequest(1, first.continuationToken()))
                        .toCompletableFuture()
                        .join();
        assertEquals(1, second.items().size());
        assertNull(second.continuationToken());
        assertNotEquals(first.items().getFirst().rid(), second.items().getFirst().rid());
        assertCapacity(second.items().getFirst().capacity(), 0, 0, 0, 0, 0, 0, 0, 0);
        for (var d : List.of(first.items().getFirst(), second.items().getFirst())) {
            var original = descriptor(owner, d.rid().toString(), 1);
            assertEquals(original.descriptorRevision(), d.descriptorRevision());
            assertEquals(original.lifecycleGeneration(), d.lifecycleGeneration());
            assertEquals(original.ownerId(), d.ownerId());
            assertEquals(original.leaseGeneration(), d.leaseGeneration());
        }
    }

    @Test
    void malformedCapacityRecordRemainsDecoderFailureWithMetrics() {
        var provider = new ZLinkInMemoryProviderLocationStore();
        var repository = new ZLinkProviderLocationRepository(provider);
        var owner =
                assertInstanceOf(
                                ZLinkOwnerLeaseClaimed.class,
                                repository
                                        .claimOwnerLease("corrupt-usage-owner", Duration.ofHours(1))
                                        .toCompletableFuture()
                                        .join())
                        .token();
        try (var observation = repository.registerCapacityObservation()) {
            repository
                    .updateMeshNode(
                            descriptor(owner, "usage-a", 0), ZLinkLocationWriteIntent.NEW_CLAIM)
                    .toCompletableFuture()
                    .join();
            provider.write(
                            new systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new systems.zlink.framework.locationprovider
                                                    .ZLinkStorePut(
                                                    new systems.zlink.framework.locationprovider
                                                            .ZLinkStoreKey(
                                                            "zlink:v11:capacity:usage:usage-a"),
                                                    "{"
                                                            .getBytes(
                                                                    java.nio.charset
                                                                            .StandardCharsets
                                                                            .UTF_8),
                                                    null))),
                            () -> false)
                    .toCompletableFuture()
                    .join();
            var failure =
                    assertThrows(
                            java.util.concurrent.CompletionException.class,
                            () ->
                                    repository
                                            .listMeshNodes("usage", new ZLinkPageRequest(10, null))
                                            .toCompletableFuture()
                                            .join());
            assertInstanceOf(IllegalStateException.class, failure.getCause());
        }
    }

    private static ZLinkMeshNodeDescriptor only(ZLinkProviderLocationRepository repository) {
        var page =
                repository
                        .listMeshNodes("usage", new ZLinkPageRequest(10, null))
                        .toCompletableFuture()
                        .join();
        assertEquals(1, page.items().size());
        return page.items().getFirst();
    }

    private static ZLinkObjectReservation reserve(
            ZLinkProviderLocationRepository repository,
            ZLinkMeshNodeDescriptor descriptor,
            ZLinkLocationOwnerToken owner,
            ZLinkPlacementObjectKind kind,
            String type,
            String id) {
        var key =
                kind == ZLinkPlacementObjectKind.ACTOR
                        ? ZLinkAuthorityKeyCodec.actor(id)
                        : ZLinkAuthorityKeyCodec.spot(id);
        var capacity =
                kind == ZLinkPlacementObjectKind.ACTOR
                        ? new ZLinkPlacementCapacityBundle(1, 0, Optional.empty())
                        : ZLinkPlacementCapacityBundle.spot(kind, type, 1);
        return assertInstanceOf(
                        ZLinkObjectReserved.class,
                        repository
                                .reserve(
                                        new ZLinkObjectReservationRequest(
                                                kind,
                                                key,
                                                type,
                                                "inline-v1:test",
                                                new byte[32],
                                                4,
                                                new ZLinkMeshNodeDescriptorKey(
                                                        descriptor.meshName(), descriptor.rid()),
                                                descriptor.lifecycleGeneration(),
                                                owner,
                                                new byte[] {1},
                                                capacity),
                                        () -> false)
                                .toCompletableFuture()
                                .join())
                .reservation();
    }

    private static void assertCapacity(
            ZLinkPlacementCapacity capacity,
            int actorActive,
            int actorReserved,
            int spotActive,
            int spotReserved,
            int userActive,
            int userReserved,
            int instanceActive,
            int instanceReserved) {
        assertEquals(new ZLinkCapacityUsage(actorActive, actorReserved, 5), capacity.actors());
        assertEquals(new ZLinkCapacityUsage(spotActive, spotReserved, 6), capacity.spots());
        assertEquals(
                new ZLinkCapacityUsage(userActive, userReserved, 3),
                capacity.spotTypes().stream()
                        .filter(
                                t ->
                                        t.objectKind() == ZLinkPlacementObjectKind.USER_SPOT
                                                && t.stableType().equals("room"))
                        .findFirst()
                        .orElseThrow()
                        .usage());
        assertEquals(
                new ZLinkCapacityUsage(instanceActive, instanceReserved, 4),
                capacity.spotTypes().stream()
                        .filter(
                                t ->
                                        t.objectKind() == ZLinkPlacementObjectKind.INSTANCE_SPOT
                                                && t.stableType().equals("dungeon"))
                        .findFirst()
                        .orElseThrow()
                        .usage());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
            ZLinkLocationOwnerToken owner, String rid, int publishedUsage) {
        return new ZLinkMeshNodeDescriptor(
                "usage",
                RoutingId.from(rid),
                7,
                11,
                "inproc://usage",
                Map.of(),
                1,
                List.of(
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.ACTOR,
                                "player",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                0),
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.USER_SPOT,
                                "room",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                3),
                        new ZLinkObjectCapability(
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                "dungeon",
                                ZLinkObjectMaintenancePolicyKind.DISABLED,
                                false,
                                4)),
                ZLinkMeshNodeObjectRole.SERVER,
                Optional.of("usage-entry"),
                100,
                new ZLinkPlacementCapacity(
                        new ZLinkCapacityUsage(publishedUsage, publishedUsage, 5),
                        new ZLinkCapacityUsage(publishedUsage, publishedUsage, 6),
                        List.of(
                                new ZLinkSpotTypeCapacity(
                                        ZLinkPlacementObjectKind.USER_SPOT,
                                        "room",
                                        new ZLinkCapacityUsage(publishedUsage, publishedUsage, 3)),
                                new ZLinkSpotTypeCapacity(
                                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                        "dungeon",
                                        new ZLinkCapacityUsage(
                                                publishedUsage, publishedUsage, 4)))),
                new ZLinkActivationConcurrency(0, 64),
                Optional.empty(),
                ZLinkFrameworkRuntimeState.SERVING,
                "usage-security",
                owner.ownerId(),
                owner.leaseGeneration(),
                Instant.parse("2026-10-30T00:00:00Z"));
    }
}
