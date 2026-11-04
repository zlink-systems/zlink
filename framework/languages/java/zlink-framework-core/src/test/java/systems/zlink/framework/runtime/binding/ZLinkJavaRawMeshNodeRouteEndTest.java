package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

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
            var port = node.port;
            // No pump runs: a new-route handshake is already committed when the old
            // selected-route observation is delivered.
            try (var router = port.openRouter(localRid)) {
                node.router = router;
                var topology =
                        new ZLinkServiceTopologyRegistry(descriptor(localRid), port.stateLane());
                node.topology = topology;
                topology.observeChanges(node::publishPeerObservation);
                assertEquals(
                        ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                        topology.admit(
                                descriptor(peerRid),
                                connectionId,
                                ServiceWireConstants.COMMAND_ADMIT));
                var liveness = node.liveness;
                liveness.admit(peerRid, connectionId, 0);
                long probeId = liveness.tick(0).probes().getFirst().probeId();
                assertTrue(liveness.acknowledge(peerRid, connectionId, probeId, 1));
                Map<RoutingId, ZLinkJavaRawMeshNode.HelloExchange> announced = node.helloExchanges;
                var exchange =
                        new ZLinkJavaRawMeshNode.HelloExchange(
                                peerRid,
                                admittedGeneration,
                                java.util.concurrent.CompletableFuture.completedFuture(null),
                                null);
                announced.put(peerRid, exchange);
                node.selectedRoutes = Map.of(peerRid, endedGeneration);

                node.observeTransportState(ZLinkJavaSocketReceivePoller.ROUTE_CHANGED);

                boolean retained = endedGeneration != admittedGeneration;
                assertEquals(retained, topology.peer(peerRid).isPresent());
                // 05-transport-liveness.ko.md:228 (§5): ACK does not own Ready.
                assertEquals(retained, liveness.connection(peerRid) != null);
                assertEquals(
                        retained, topology.peer(peerRid).filter(topology::isReady).isPresent());
                assertEquals(retained ? exchange : null, announced.get(peerRid));
                node.router = null;
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
}
