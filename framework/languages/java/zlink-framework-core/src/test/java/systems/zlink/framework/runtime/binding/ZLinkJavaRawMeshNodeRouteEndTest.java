package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.util.List;
import java.util.Map;

final class ZLinkJavaRawMeshNodeRouteEndTest {
    @ParameterizedTest
    @ValueSource(longs = {1, 2})
    void routeEndOnlyRetiresItsOwnAdmission(long endedGeneration) throws Exception {
        RoutingId localRid = RoutingId.from("route-end-local");
        RoutingId peerRid = RoutingId.from("route-end-peer");
        long admittedGeneration = 2;
        String connectionId = ZLinkJavaRawMeshNode.routeConnectionId(admittedGeneration);
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            // No pump runs: a new-route handshake is already committed when the old
            // selected-route observation is delivered.
            try (var router = port.openRouter(localRid)) {
                setField(node, "router", router);
                var topology = new ZLinkServiceTopologyRegistry(descriptor(localRid));
                setField(node, "topology", topology);
                assertEquals(
                        ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                        topology.admit(descriptor(peerRid), connectionId));
                var liveness = (ZLinkServiceLivenessRegistry) field(node, "liveness");
                liveness.admit(peerRid, connectionId, 0);
                long probeId = liveness.tick(0).probes().getFirst().probeId();
                assertTrue(liveness.acknowledge(peerRid, connectionId, probeId, 1));
                Map<RoutingId, String> ready = mapField(node, "admissionControlReadyConnections");
                ready.put(peerRid, connectionId);
                Map<RoutingId, Long> announced = mapField(node, "announcedRouteGenerations");
                announced.put(peerRid, admittedGeneration);
                setField(node, "selectedRoutes", Map.of(peerRid, endedGeneration));

                var observe = ZLinkJavaRawMeshNode.class.getDeclaredMethod("observeSelectedRoutes");
                observe.setAccessible(true);
                observe.invoke(node);

                boolean retained = endedGeneration != admittedGeneration;
                assertEquals(retained, topology.peer(peerRid).isPresent());
                // 05-transport-liveness.ko.md:228 (§5): ACK does not own Ready.
                assertEquals(retained, liveness.connection(peerRid) != null);
                assertEquals(retained ? connectionId : null, ready.get(peerRid));
                assertEquals(
                        retained ? Long.valueOf(admittedGeneration) : null, announced.get(peerRid));
                setField(node, "router", null);
            }
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                rid,
                1,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                1,
                1,
                0,
                0);
    }

    private static Object field(ZLinkJavaRawMeshNode node, String name) throws Exception {
        var field = ZLinkJavaRawMeshNode.class.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(node);
    }

    @SuppressWarnings("unchecked")
    private static <K, V> Map<K, V> mapField(ZLinkJavaRawMeshNode node, String name)
            throws Exception {
        return (Map<K, V>) field(node, name);
    }

    private static void setField(ZLinkJavaRawMeshNode node, String name, Object value)
            throws Exception {
        var field = ZLinkJavaRawMeshNode.class.getDeclaredField(name);
        field.setAccessible(true);
        field.set(node, value);
    }
}
