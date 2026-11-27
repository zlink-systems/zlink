package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.List;
import java.util.Map;

final class ZLinkJavaRawMeshNodeRelocationReplyTest {
    @Test
    void relocationForwardRequiresServiceAdmissionOnTheExactPeerGeneration() throws Exception {
        var localRid = RoutingId.from("relocation-barrier-source");
        var peerRid = RoutingId.from("relocation-barrier-target");
        try (var context = Zlink.createContext();
                var port = new ZLinkJavaRawServicePort(context)) {
            var router = port.openRouter(localRid);
            var local = descriptor(localRid, 1);
            var peer = descriptor(peerRid, 2);
            var topology = new ZLinkServiceTopologyRegistry(local, port.stateLane());
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    topology.admit(peer, "1", ServiceWireConstants.COMMAND_HELLO));
            try (var source =
                            ZLinkJavaRawMeshNodeTesting.create(
                                    "mesh",
                                    port,
                                    router,
                                    local,
                                    topology,
                                    Map.of(peerRid, 1L),
                                    Runnable::run);
                    var part = Message.from("payload")) {
                var target =
                        new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                "target-spot", 1, peerRid, peer.lifecycleGeneration(), 1, 1);
                var stale =
                        new ZLinkServiceM6BWireCodec.SpotMessage(
                                false, 0, null, "source-spot", target);
                var parts = List.of(part);
                assertFalse(
                        source.forwardRelocationSpot(
                                stale, target, null, parts, ignored -> {}, ignored -> {}),
                        "a HELLO descriptor alone must not cross the relocation route barrier");
                assertEquals(
                        ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                        topology.admit(peer, "1", ServiceWireConstants.COMMAND_ADMIT));
                var wrongGeneration =
                        new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                "target-spot", 1, peerRid, peer.lifecycleGeneration() + 1, 1, 1);
                assertFalse(
                        source.forwardRelocationSpot(
                                stale, wrongGeneration, null, parts, ignored -> {}, ignored -> {}));
                assertTrue(
                        source.forwardRelocationSpot(
                                stale, target, null, parts, ignored -> {}, ignored -> {}),
                        "the matching admitted peer may cross the relocation route barrier");
            }
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long generation) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                rid,
                generation,
                1,
                "tcp://127.0.0.1:12345",
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                1,
                100,
                0,
                0);
    }
}
