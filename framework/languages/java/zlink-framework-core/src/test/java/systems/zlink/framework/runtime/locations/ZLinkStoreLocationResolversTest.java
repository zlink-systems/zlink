package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;

final class ZLinkStoreLocationResolversTest {
    private static final Instant NOW =
        Instant.parse("2026-07-27T00:00:00Z");
    private static final RoutingId NODE = RoutingId.from("node-a");

    @Test
    void positiveReadyAuthorityIsCachedButMissingAuthorityIsNot() {
        AtomicInteger reads = new AtomicInteger();
        AtomicReference<Object> current = new AtomicReference<>(
            readySpotSnapshot());
        ZLinkLocationRepository store = (ZLinkLocationRepository)
            Proxy.newProxyInstance(
                getClass().getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "read" -> {
                        reads.incrementAndGet();
                        yield CompletableFuture.completedFuture(current.get());
                    }
                    case "readOwnerLease" ->
                        CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseFound(
                                new ZLinkLocationOwnerToken(
                                    "owner-a",
                                    ((ZLinkAuthoritySnapshot) current.get())
                                        .ownerLeaseGeneration()),
                                NOW.plusSeconds(30),
                                NOW));
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkLocationOptions cachedOptions = new ZLinkLocationOptions();
        cachedOptions.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var cached = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(store),
            cachedOptions);

        assertEquals("room-a", cached.resolveSpot("room-a")
            .toCompletableFuture().join().spotId());
        assertEquals("room-a", cached.resolveSpot("room-a")
            .toCompletableFuture().join().spotId());
        assertEquals(1, reads.get());

        ZLinkLocationOptions uncachedOptions = new ZLinkLocationOptions();
        uncachedOptions.setRouteCacheMaxAge(Duration.ZERO);
        current.set(new ZLinkAuthorityMissing(NOW));
        var misses = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(store),
            uncachedOptions);
        assertNull(misses.resolveSpot("room-a").toCompletableFuture().join());
        assertNull(misses.resolveSpot("room-a").toCompletableFuture().join());
        assertEquals(3, reads.get());
    }

    @Test
    void expiredExactOwnerLeaseRejectsAnOtherwiseReadyRoute() {
        ZLinkLocationRepository store = (ZLinkLocationRepository)
            Proxy.newProxyInstance(
                getClass().getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "read" -> CompletableFuture.completedFuture(
                        readySpotSnapshot());
                    case "readOwnerLease" ->
                        CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseMissing());
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        var resolvers = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(store),
            new ZLinkLocationOptions());

        assertNull(
            resolvers.resolveSpot("room-a").toCompletableFuture().join());
    }

    @Test
    void messageFollowInvalidatesOnlyTheExactCachedRouteFence() {
        AtomicInteger reads = new AtomicInteger();
        AtomicReference<Object> current = new AtomicReference<>(
            readySpotSnapshot());
        ZLinkLocationRepository store = (ZLinkLocationRepository)
            Proxy.newProxyInstance(
                getClass().getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "read" -> {
                        reads.incrementAndGet();
                        yield CompletableFuture.completedFuture(current.get());
                    }
                    case "readOwnerLease" ->
                        CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseFound(
                                new ZLinkLocationOwnerToken("owner-a", 7),
                                NOW.plusSeconds(30),
                                NOW));
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setRouteCacheMaxAge(Duration.ofSeconds(10));
        var resolvers = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(store), options);

        resolvers.resolveSpot("room-a").toCompletableFuture().join();
        var oldRoute = new ZLinkServiceMessageFollowWireCodec.SpotRoute(
            "room-a", 5, NODE, 11, 13, 7);
        var wrongLease = new ZLinkServiceMessageFollowWireCodec.SpotRoute(
            "room-a", 5, NODE, 11, 13, 8);
        assertFalse(resolvers.invalidateRouteIfMatches(wrongLease));
        assertEquals(1, reads.get());

        assertTrue(resolvers.invalidateRouteIfMatches(oldRoute));
        current.set(readySpotSnapshot(9, 21, 7, 17, "v2"));
        resolvers.resolveSpot("room-a").toCompletableFuture().join();
        assertEquals(2, reads.get());

        // A delayed notice from the previous owner must not erase v2.
        assertFalse(resolvers.invalidateRouteIfMatches(oldRoute));
        assertEquals(
            9,
            resolvers.resolveSpot("room-a")
                .toCompletableFuture().join().spotGeneration());
        assertEquals(2, reads.get());
    }

    @Test
    void autoConnectPeerDiscoveryExcludesExpiredMeshOwnerLeases() {
        ZLinkMeshNodeDescriptor live = meshNodeDescriptor(
            "mesh", RoutingId.from("live"), "live-owner");
        ZLinkMeshNodeDescriptor expired = meshNodeDescriptor(
            "mesh", RoutingId.from("expired"), "expired-owner");
        ZLinkLocationRepository store = (ZLinkLocationRepository)
            Proxy.newProxyInstance(
                getClass().getClassLoader(),
                new Class<?>[] {ZLinkLocationRepository.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "listMeshNodes" -> CompletableFuture.completedFuture(
                        new ZLinkLocationPage<>(List.of(live, expired), null));
                    case "readOwnerLease" -> "live-owner".equals(arguments[0])
                        ? CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseFound(
                                new ZLinkLocationOwnerToken("live-owner", 1),
                                NOW.plusSeconds(30), NOW))
                        : CompletableFuture.completedFuture(
                            new ZLinkOwnerLeaseMissing());
                    default -> throw new UnsupportedOperationException(
                        method.getName());
                });
        var resolvers = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(store),
            new ZLinkLocationOptions());

        var peers = resolvers.listPeers(
                ZLinkAutoConnectType.ROUTE_MESH,
                "mesh",
                ZLinkLocationRole.ROUTER)
            .toCompletableFuture()
            .join();

        assertEquals(List.of(RoutingId.from("live")),
            peers.stream().map(ZLinkAutoConnectPeer::nodeRid).toList());
    }

    private static ZLinkMeshNodeDescriptor meshNodeDescriptor(
        String meshName,
        RoutingId rid,
        String ownerId) {
        return new ZLinkMeshNodeDescriptor(
            meshName,
            rid,
            1,
            1,
            "tcp://127.0.0.1:7000",
            Map.of("orders", 100),
            1,
            List.of(),
            ZLinkMeshNodeObjectRole.NONE,
            Optional.empty(),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 1),
            Optional.empty(),
            systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState.SERVING,
            "security",
            ownerId,
            1,
            NOW);
    }

    private static ZLinkAuthoritySnapshot readySpotSnapshot() {
        return readySpotSnapshot(5, 13, 7, 11, "v1");
    }

    private static ZLinkAuthoritySnapshot readySpotSnapshot(
        long objectGeneration,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration,
        long nodeGeneration,
        String storeVersion) {
        byte[] payload = new ZLinkServiceAuthorityPayloadCodec().encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "RoomSpot",
            "room-a",
            "owner-a",
            ownerLeaseGeneration,
            "game",
            NODE,
            nodeGeneration);
        return new ZLinkAuthoritySnapshot(
            storeVersion,
            payload,
            objectGeneration,
            authorityOwnerGeneration,
            "owner-a",
            ownerLeaseGeneration,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.USER_SPOT,
                "RoomSpot",
                new ZLinkMeshNodeDescriptorKey("game", NODE),
                nodeGeneration,
                ZLinkPlacementCapacityBundle.spot(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    "RoomSpot",
                    1)),
            Optional.empty(),
            NOW);
    }
}
