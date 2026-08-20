package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityMissing;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocation;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementAllocationState;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkActorJoinAuthorityStoreTest {
    private static final String ACTOR_ID = "actor-a";
    private static final RoutingId NODE = RoutingId.from("node-a");

    @Test
    void matchingActiveActorRowResolvesStoreStableType() {
        ZLinkActorRuntime runtime = runtime(reads(row()));
        var authority = runtime.readActorJoinAuthority(ACTOR_ID)
            .toCompletableFuture().join();

        assertEquals("canonical-type", authority.stableType());
        assertEquals(7L, authority.objectGeneration());
        assertEquals(NODE, authority.ownerNodeRid());
        assertEquals(-9L, authority.ownerNodeGeneration(),
            "opaque lifecycle generations may be negative after unsigned decode");
    }

    @Test
    void missingAndUnreadableRowsHaveTypedTerminals() {
        assertKind(reads(new ZLinkAuthorityMissing(Instant.now())),
            ZLinkFrameworkErrorKind.NOT_FOUND);
        assertKind(failingReads(), ZLinkFrameworkErrorKind.UNAVAILABLE);
    }

    @Test
    void incompleteFenceIsProtocolError() {
        ZLinkAuthoritySnapshot invalid = new ZLinkAuthoritySnapshot(
            "v1", new byte[0], 7L, 2L, "owner", 3L,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.PENDING,
                ZLinkPlacementObjectKind.ACTOR,
                "canonical-type",
                new ZLinkMeshNodeDescriptorKey("mesh", NODE),
                -9L,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
        assertKind(reads(invalid), ZLinkFrameworkErrorKind.PROTOCOL_ERROR);
    }

    private static void assertKind(
        ZLinkLocationRepository store,
        ZLinkFrameworkErrorKind expected) {
        java.util.concurrent.CompletionException completion = assertThrows(
            java.util.concurrent.CompletionException.class,
            () -> runtime(store).readActorJoinAuthority(ACTOR_ID)
                .toCompletableFuture().join());
        ZLinkFrameworkException error =
            (ZLinkFrameworkException) completion.getCause();
        assertEquals(expected, error.kind());
    }

    private static ZLinkActorRuntime runtime(ZLinkLocationRepository store) {
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> "routingId".equals(method.getName())
                ? NODE : primitiveDefault(method.getReturnType()));
        ZLinkActorRuntime runtime = new ZLinkActorRuntime(
            node, Map.<String, Class<? extends ZLinkActorFactory>>of(),
            Duration.ofSeconds(1), new ZLinkJsonMessageSerializer());
        runtime.setDirectJoinRelocationStores(store);
        return runtime;
    }

    private static ZLinkLocationRepository reads(Object result) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> "read".equals(method.getName())
                ? CompletableFuture.completedFuture(result)
                : primitiveDefault(method.getReturnType()));
    }

    private static ZLinkLocationRepository failingReads() {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> "read".equals(method.getName())
                ? CompletableFuture.failedFuture(new IllegalStateException("store down"))
                : primitiveDefault(method.getReturnType()));
    }

    private static ZLinkAuthoritySnapshot row() {
        return new ZLinkAuthoritySnapshot(
            "v1", new byte[0], 7L, 2L, "owner", 3L,
            new ZLinkPlacementAllocation(
                ZLinkPlacementAllocationState.ACTIVE,
                ZLinkPlacementObjectKind.ACTOR,
                "canonical-type",
                new ZLinkMeshNodeDescriptorKey("mesh", NODE),
                -9L,
                ZLinkPlacementCapacityBundle.actor(1)),
            Instant.now());
    }

    private static Object primitiveDefault(Class<?> type) {
        if (type == boolean.class) return false;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        return null;
    }
}
