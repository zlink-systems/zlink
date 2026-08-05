package systems.zlink.framework.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkLiveLocationRows;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntimeQueryService;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.internal.locations.*;

final class LocationStoreContractTest {
    private static final Instant STORE_NOW = Instant.parse("2026-07-03T00:00:00Z");
    private static final RoutingId NODE_A = RoutingId.from("node-a");

    @Test
    void registrationKeepsOneUnifiedProviderBoundary() {
        ZLinkInMemoryLocationStore store = newStore();
        ZLinkProviderLocationRepository repository = repository(store);
        ZLinkRegisteredLocationStores stores =
            ZLinkRegisteredLocationStores.fromUnified(repository);

        assertSame(repository, stores.unifiedStore());
        assertTrue(store instanceof systems.zlink.framework.locationprovider
            .ZLinkLocationStore);
    }

    @Test
    void descriptorPublicationUsesOwnerLeaseAndLifecycleFences() throws Exception {
        ZLinkInMemoryLocationStore store = newStore();
        ZLinkProviderLocationRepository repository = repository(store);
        ZLinkLocationOwnerToken ownerA = ((ZLinkOwnerLeaseClaimed)
            repository.claimOwnerLease("owner-a", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();
        ZLinkLocationOwnerToken ownerB = ((ZLinkOwnerLeaseClaimed)
            repository.claimOwnerLease("owner-b", Duration.ofSeconds(30))
                .toCompletableFuture().get()).token();

        ZLinkLocationWriteResult stored = repository.updateMeshNode(
                descriptor(ownerA, 1),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().get();
        ZLinkLocationWriteResult conflict = repository.updateMeshNode(
                descriptor(ownerB, 1),
                ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().get();
        ZLinkLocationPage<ZLinkMeshNodeDescriptor> page =
            repository.listMeshNodes(
                "play",
                ZLinkPageRequest.firstPage())
            .toCompletableFuture().get();

        assertEquals(ZLinkLocationWriteStatus.STORED, stored.status());
        assertEquals(1, stored.generation());
        assertEquals(ZLinkLocationWriteStatus.REJECTED_CONFLICT, conflict.status());
        assertEquals(List.of(NODE_A), page.items().stream()
            .map(ZLinkMeshNodeDescriptor::rid).toList());
    }

    @Test
    void runtimeQueryProjectsOnlyConfiguredLiveMeshDescriptors() throws Exception {
        ZLinkInMemoryLocationStore store = newStore();
        ZLinkProviderLocationRepository repository = repository(store);
        ZLinkRegisteredLocationStores stores =
            ZLinkRegisteredLocationStores.fromUnified(repository);
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        ZLinkLocationRuntime runtime = new ZLinkLocationRuntime(
            stores,
            Duration.ofSeconds(30),
            Duration.ofSeconds(5));
        runtime.start(NODE_A).toCompletableFuture().get();
        try (runtime) {
            repository.updateMeshNode(
                    descriptor(runtime.currentOwnerToken(), 1),
                    ZLinkLocationWriteIntent.NEW_CLAIM)
                .toCompletableFuture().get();
            ZLinkLocationRuntimeQueryService query =
                new ZLinkLocationRuntimeQueryService(
                    stores,
                    runtime,
                    options,
                    ZLinkLiveLocationRows.create(stores, options),
                    List.of("play"));

            ZLinkLocationRuntimeStatus status = query.getStatus()
                .toCompletableFuture().get();
            ZLinkLocationPage<ZLinkLocationTopologyEntry> topology =
                query.listTopology(
                        ZLinkLocationTopologyFilter.all(),
                        ZLinkPageRequest.firstPage())
                    .toCompletableFuture().get();

            assertTrue(status.storeHealthy());
            assertTrue(status.ownerLeaseHealthy());
            assertFalse(status.watchEnabled());
            assertEquals(1, topology.items().size());
            assertEquals(NODE_A, topology.items().getFirst().nodeRid());
        }
    }

    @Test
    void removedRawRowContractsAreNotPublicTypes() {
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkPeerLocation"));
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkSpotLocation"));
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkActorLocation"));
        assertThrows(ClassNotFoundException.class, () ->
            Class.forName("systems.zlink.framework.runtime.internal.locations.ZLinkRouteLocation"));
    }

    private static ZLinkInMemoryLocationStore newStore() {
        return new ZLinkInMemoryLocationStore(
            Clock.fixed(STORE_NOW, ZoneOffset.UTC));
    }

    private static ZLinkProviderLocationRepository repository(
        ZLinkInMemoryLocationStore store) {
        return new ZLinkProviderLocationRepository(store);
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        ZLinkLocationOwnerToken owner,
        long lifecycleGeneration) {
        return new ZLinkMeshNodeDescriptor(
            "play",
            NODE_A,
            lifecycleGeneration,
            1,
            "tcp://127.0.0.1:5001",
            Map.of("play", 100),
            1,
            List.of(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.ACTOR,
                "player",
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0)),
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of("play-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 8),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 128),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "security",
            owner.ownerId(),
            owner.leaseGeneration(),
            STORE_NOW);
    }
}
