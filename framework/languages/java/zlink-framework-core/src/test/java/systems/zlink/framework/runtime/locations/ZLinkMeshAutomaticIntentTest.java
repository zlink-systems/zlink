package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.Set;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;

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

    private static ZLinkAutoConnectPlanner.Target target() {
        return new ZLinkAutoConnectPlanner.Target(
                "peer", PEER, ZLinkLocationRole.ROUTER, ENDPOINT, Map.of(), "peer-owner", 1);
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
