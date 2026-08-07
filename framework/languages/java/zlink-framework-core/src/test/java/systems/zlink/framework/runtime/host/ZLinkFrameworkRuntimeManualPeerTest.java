package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Instant;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

final class ZLinkFrameworkRuntimeManualPeerTest {
    @Test
    void startupObjectPeerUsesDescriptorAdmissionFence() {
        RoutingId localRid = RoutingId.from("local-node");
        RoutingId targetRid = RoutingId.from("target-node");
        String endpoint = "inproc://target-node";
        ZLinkMeshNodeDescriptor target = new ZLinkMeshNodeDescriptor(
            "mesh",
            targetRid,
            7,
            1,
            endpoint,
            Map.of(),
            1,
            List.of(),
            ZLinkMeshNodeObjectRole.CLIENT,
            Optional.empty(),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 0),
                new ZLinkCapacityUsage(0, 0, 0),
                List.of()),
            new ZLinkActivationConcurrency(0, 8),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "target-security",
            "target-owner",
            1,
            Instant.parse("2026-01-01T00:00:00Z"));
        RecordingMeshNode source = new RecordingMeshNode(localRid);

        boolean connected = ZLinkFrameworkRuntime.connectManualObjectPeer(
            source,
            new MeshNodeRegistration.Peer(endpoint, null),
            List.of(target));

        assertTrue(connected);
        assertEquals(endpoint, source.endpoint);
        assertEquals(targetRid, source.routingId);
        assertEquals(7L, source.lifecycleGeneration);
        assertEquals("target-security", source.securityIdentity);
    }

    private static final class RecordingMeshNode implements ZLinkInternalMeshNode {
        private final RoutingId localRid;
        private String endpoint;
        private RoutingId routingId;
        private long lifecycleGeneration;
        private String securityIdentity;

        private RecordingMeshNode(RoutingId localRid) {
            this.localRid = localRid;
        }

        @Override public String name() { return "source"; }
        @Override public void setBind(String value) { }
        @Override public void addChannel(String value) { }
        @Override public void setChannelWeight(String value, int weight) { }
        @Override public void setRoutingId(RoutingId value) { }
        @Override public void start() { }
        @Override public long connectPeer(String value) { return 1L; }
        @Override public long connectPeer(String value, RoutingId expected) {
            return 1L;
        }
        @Override public long replacePeerConnection(
            String value,
            RoutingId expected,
            long generation,
            String security) {
            endpoint = value;
            routingId = expected;
            lifecycleGeneration = generation;
            securityIdentity = security;
            return 2L;
        }
        @Override public MeshNodeStatus status() {
            return new MeshNodeStatus(
                MeshNodeState.READY,
                localRid,
                "mesh",
                "inproc://source",
                1L,
                1L,
                1,
                1,
                1,
                0,
                0L,
                0L,
                0L,
                0,
                0L);
        }
        @Override public List<MeshPeerEntry> peers() { return List.of(); }
        @Override public List<Long> connectionIntentIds() { return List.of(); }
        @Override public void startDispatch(Consumer<ZLinkMeshDispatchRecord> receiver) { }
        @Override public void close() { }
    }
}
