package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkMeshAutomaticIntentTest {
    private static final RoutingId PEER = RoutingId.from("automatic-intent-peer");
    private static final String ENDPOINT = "inproc://automatic-intent-peer";

    @ParameterizedTest
    @ValueSource(ints = {0, 1, 2, 3})
    void existingSelectedRouteOrManualIntentPreventsAutomaticDial(int existing) {
        var routes = new AtomicBoolean(existing == 1);
        var dials = new AtomicInteger();
        var executor =
                new ZLinkLocationAutoConnectHost.MeshNodeExecutor(
                        node(routes, dials),
                        existing == 2 ? Set.of(ENDPOINT) : Set.of(),
                        existing == 3 ? Map.of("inproc://manual-other-endpoint", PEER) : Map.of());
        assertEquals(existing == 0, executor.connect(target()));
        assertEquals(existing == 0 ? 1 : 0, dials.get());
    }

    @Test
    void lostInboundRouteAllowsAutomaticIntentOnTheNextReconcile() {
        var routes = new AtomicBoolean(true);
        var dials = new AtomicInteger();
        var executor =
                new ZLinkLocationAutoConnectHost.MeshNodeExecutor(
                        node(routes, dials), Set.of(), Map.of());
        assertFalse(executor.connect(target()));
        assertEquals(0, dials.get());
        routes.set(false);
        assertTrue(executor.connect(target()));
        assertEquals(1, dials.get());
    }

    @Test
    void descriptorObjectRoleReachesAdmissionExpectation() {
        var peer =
                new ZLinkAutoConnectPeer(
                        ZLinkAutoConnectType.ROUTE_MESH,
                        "mesh",
                        PEER,
                        ZLinkLocationRole.ROUTER,
                        ENDPOINT,
                        100,
                        false,
                        1,
                        Map.of(),
                        List.of(),
                        "owner",
                        1,
                        Instant.EPOCH,
                        ZLinkMeshNodeObjectRole.CLIENT,
                        false);
        var local =
                new ZLinkAutoConnectPlanner.Local(
                        ZLinkAutoConnectType.ROUTE_MESH,
                        "mesh",
                        ZLinkLocationRole.ROUTER,
                        RoutingId.from("a"),
                        "inproc://local");
        var observed = new AtomicReference<ZLinkMeshNodeObjectRole>();
        var node =
                (ZLinkInternalMeshNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalMeshNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalMeshNode.class},
                                (proxy, method, args) -> {
                                    if (method.getName()
                                            .equals("observePeerAdmissionExpectation")) {
                                        observed.set((ZLinkMeshNodeObjectRole) args[4]);
                                        return null;
                                    }
                                    throw new UnsupportedOperationException(method.getName());
                                });
        var targets = ZLinkAutoConnectPlanner.computeDesired(local, List.of(peer));
        assertEquals(1, targets.size());
        new ZLinkLocationAutoConnectHost.MeshNodeExecutor(node, Set.of(), Map.of())
                .observeAdmissionExpectation(targets.values().iterator().next());
        assertEquals(ZLinkMeshNodeObjectRole.CLIENT, observed.get());
    }

    private static ZLinkAutoConnectPlanner.Target target() {
        return new ZLinkAutoConnectPlanner.Target(
                "peer",
                PEER,
                ZLinkLocationRole.ROUTER,
                ENDPOINT,
                Map.of(),
                "peer-owner",
                1,
                ZLinkMeshNodeObjectRole.NONE);
    }

    private static ZLinkInternalMeshNode node(AtomicBoolean routes, AtomicInteger dials) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, args) -> {
                            if (method.getName().equals("isPeerTransportConnected"))
                                return routes.get();
                            if (method.getName().equals("replacePeerConnection")) {
                                dials.incrementAndGet();
                                return 1L;
                            }
                            if (method.isDefault())
                                return InvocationHandler.invokeDefault(proxy, method, args);
                            throw new UnsupportedOperationException(method.getName());
                        });
    }
}
