package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CopyOnWriteArrayList;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityPage;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

final class ZLinkStatefulAuthorityRouteRuntimeTest {
    private static final String MESH_NAME = "game";
    private static final String STABLE_TYPE = "game.room";
    private static final String OWNER_ID = "owner-a";
    private static final long OWNER_LEASE_GENERATION = 31;
    private static final long NODE_GENERATION = 17;
    private static final long OBJECT_GENERATION = 5;
    private static final long AUTHORITY_GENERATION = 7;
    private static final RoutingId SPOT_RID =
        RoutingId.from("instance-room");
    private static final RoutingId NODE_RID =
        RoutingId.from("node-a");
    private static final String AUTHORITY_KEY =
        ZLinkAuthorityKeyCodec.spot(SPOT_RID.toString());

    @Test
    void pendingInstanceAuthorityRegistersIntentBeforeReadyRoutePublication() {
        var codec = new ZLinkServiceAuthorityPayloadCodec();
        var entries = new java.util.concurrent.atomic.AtomicReference<>(
            List.of(entry(
                "v1",
                ZLinkPlacementAllocationState.PENDING,
                codec.encodeInstance(
                    ZLinkServiceAuthorityPayloadCodec.State.CREATING,
                    STABLE_TYPE,
                    SPOT_RID.toString(),
                    OWNER_ID,
                    OWNER_LEASE_GENERATION,
                    MESH_NAME,
                    NODE_RID,
                    NODE_GENERATION))));
        ZLinkLocationRepository store = authorityStore(entries);
        var recorded = new RecordedNode();
        var failures = new CopyOnWriteArrayList<Throwable>();

        try (var runtime = new ZLinkStatefulAuthorityRouteRuntime(
                 store,
                 Map.of(MESH_NAME, recorded.proxy()),
                 Duration.ofHours(1),
                 failures::add)) {
            runtime.start().toCompletableFuture().join();

            assertEquals(1, recorded.registered.size());
            assertTrue(recorded.remembered.isEmpty());
            assertFence(recorded.registered.getFirst());

            entries.set(List.of(entry(
                "v2",
                ZLinkPlacementAllocationState.ACTIVE,
                codec.encodeInstance(
                    ZLinkServiceAuthorityPayloadCodec.State.READY,
                    STABLE_TYPE,
                    SPOT_RID.toString(),
                    OWNER_ID,
                    OWNER_LEASE_GENERATION,
                    MESH_NAME,
                    NODE_RID,
                    NODE_GENERATION))));
            runtime.reconcile().toCompletableFuture().join();

            await(() -> recorded.remembered.size() == 1
                && recorded.registered.size() == 2
                && recorded.forgottenIntents.size() == 1);

            entries.set(List.of());
            runtime.reconcile().toCompletableFuture().join();

            await(() -> recorded.forgottenRoutes.size() == 1
                && recorded.forgottenIntents.size() == 2);
            assertTrue(failures.isEmpty(), failures.toString());
        }
    }

    private static ZLinkAuthorityEntry entry(
        String storeVersion,
        ZLinkPlacementAllocationState state,
        byte[] payload) {
        return new ZLinkAuthorityEntry(
            AUTHORITY_KEY,
            new ZLinkAuthoritySnapshot(
                storeVersion,
                payload,
                OBJECT_GENERATION,
                AUTHORITY_GENERATION,
                OWNER_ID,
                OWNER_LEASE_GENERATION,
                new ZLinkPlacementAllocation(
                    state,
                    ZLinkPlacementObjectKind.INSTANCE_SPOT,
                    STABLE_TYPE,
                    new ZLinkMeshNodeDescriptorKey(
                        MESH_NAME,
                        NODE_RID),
                    NODE_GENERATION,
                    ZLinkPlacementCapacityBundle.spot(
                        ZLinkPlacementObjectKind.INSTANCE_SPOT,
                        STABLE_TYPE,
                        1)),
                Instant.EPOCH));
    }

    private static ZLinkLocationRepository authorityStore(
        java.util.concurrent.atomic.AtomicReference<
            List<ZLinkAuthorityEntry>> entries) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> {
                if (method.getName().equals("list")) {
                    return CompletableFuture.completedFuture(
                        new ZLinkAuthorityPage(
                            entries.get(),
                            Optional.empty()));
                }
                if (method.getDeclaringClass() == Object.class) {
                    return method.invoke(entries, arguments);
                }
                throw new UnsupportedOperationException(method.getName());
            });
    }

    private static void assertFence(
        ZLinkServiceM6BWireCodec.InstanceRouteFence fence) {
        assertEquals(NODE_RID, fence.targetNodeRid());
        assertEquals(NODE_GENERATION, fence.targetNodeGeneration());
        assertEquals(SPOT_RID.toString(), fence.targetSpotId());
        assertEquals(OBJECT_GENERATION, fence.objectGeneration());
        assertEquals(AUTHORITY_GENERATION,
            fence.authorityOwnerGeneration());
        assertEquals(OWNER_LEASE_GENERATION,
            fence.leaseGeneration());
    }

    private static void await(java.util.function.BooleanSupplier condition) {
        long deadline = System.nanoTime()
            + Duration.ofSeconds(2).toNanos();
        while (!condition.getAsBoolean()
            && System.nanoTime() < deadline) {
            java.util.concurrent.locks.LockSupport.parkNanos(
                Duration.ofMillis(5).toNanos());
        }
        assertTrue(condition.getAsBoolean());
    }

    private static final class RecordedNode {
        private final List<ZLinkInternalMeshNode.SpotAuthorityRoute>
            remembered = new CopyOnWriteArrayList<>();
        private final List<ZLinkInternalMeshNode.SpotAuthorityRoute>
            forgottenRoutes = new CopyOnWriteArrayList<>();
        private final List<ZLinkServiceM6BWireCodec.InstanceRouteFence>
            registered = new CopyOnWriteArrayList<>();
        private final List<ZLinkServiceM6BWireCodec.InstanceRouteFence>
            forgottenIntents = new CopyOnWriteArrayList<>();

        ZLinkInternalMeshNode proxy() {
            return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) -> {
                    switch (method.getName()) {
                        case "rememberSpotAuthority" ->
                            remembered.add(
                                (ZLinkInternalMeshNode.SpotAuthorityRoute)
                                    arguments[0]);
                        case "forgetSpotAuthority" ->
                            forgottenRoutes.add(
                                (ZLinkInternalMeshNode.SpotAuthorityRoute)
                                    arguments[0]);
                        case "registerInstanceIntent" ->
                            registered.add(
                                (ZLinkServiceM6BWireCodec.InstanceRouteFence)
                                    arguments[1]);
                        case "forgetInstanceIntent" ->
                            forgottenIntents.add(
                                (ZLinkServiceM6BWireCodec.InstanceRouteFence)
                                    arguments[0]);
                        case "name" -> {
                            return "recorded-node";
                        }
                        case "toString" -> {
                            return "RecordedNode";
                        }
                        case "hashCode" -> {
                            return System.identityHashCode(proxy);
                        }
                        case "equals" -> {
                            return proxy == arguments[0];
                        }
                        default -> {
                        }
                    }
                    return null;
                });
        }
    }
}
