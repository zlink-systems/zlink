package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeStatus;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;

final class ZLinkRouteMeshRuntimeOptionsRuntimeTest {
    @Test
    void liveWeightMutationUpdatesTheNodeAndPublishesOneRevision() {
        TestMeshNode node = new TestMeshNode("game", Map.of("orders", 100));
        AtomicInteger revisions = new AtomicInteger();
        var runtime = new ZLinkRouteMeshRuntimeOptionsRuntime(
            Map.of("game", node),
            revisions::incrementAndGet);

        runtime.mesh("game").setPlacementWeight(0);
        runtime.channel("game", "orders").weight(10_000);

        assertEquals(0, node.placementWeight());
        assertEquals(10_000, node.channelWeights().get("orders"));
        assertEquals(2, revisions.get());

        runtime.mesh("game").setPlacementWeight(0);
        runtime.channel("orders").weight(10_000);
        assertEquals(2, revisions.get());
    }

    @Test
    void weightRangeAndChannelIdentityAreValidatedBeforeMutation() {
        TestMeshNode game = new TestMeshNode("game", Map.of("orders", 100));
        TestMeshNode admin = new TestMeshNode("admin", Map.of("orders", 100));
        var runtime = new ZLinkRouteMeshRuntimeOptionsRuntime(
            Map.of("game", game, "admin", admin),
            () -> { });

        assertThrows(
            ZLinkConfigurationException.class,
            () -> runtime.mesh("game").setPlacementWeight(-1));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> runtime.channel("game", "orders").weight(10_001));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> runtime.channel("orders"));
        assertThrows(
            ZLinkConfigurationException.class,
            () -> runtime.channel("game", "missing"));
    }

    @Test
    void maxMessageSizeMutationDoesNotRepublishPlacementDescriptor() {
        TestMeshNode node = new TestMeshNode("game", Map.of());
        AtomicInteger revisions = new AtomicInteger();
        var runtime = new ZLinkRouteMeshRuntimeOptionsRuntime(
            Map.of("game", node),
            revisions::incrementAndGet);

        runtime.meshNode("game").maxMessageSize(4096);

        assertEquals(4096, runtime.meshNode("game").maxMessageSize());
        assertEquals(0, revisions.get());
        assertThrows(
            ZLinkConfigurationException.class,
            () -> runtime.meshNode("game").maxMessageSize(-1));
    }

    private static final class TestMeshNode implements ZLinkInternalMeshNode {
        private final String name;
        private final Map<String, Integer> weights;
        private int placementWeight = 100;
        private long maxMessageSize;

        TestMeshNode(String name, Map<String, Integer> weights) {
            this.name = name;
            this.weights = new LinkedHashMap<>(weights);
        }

        @Override
        public String name() {
            return name;
        }

        @Override
        public void setBind(String endpoint) {
        }

        @Override
        public void addChannel(String channelName) {
            weights.put(channelName, 100);
        }

        @Override
        public void setChannelWeight(String channelName, int weight) {
            weights.put(channelName, weight);
        }

        @Override
        public Map<String, Integer> channelWeights() {
            return Map.copyOf(weights);
        }

        @Override
        public int placementWeight() {
            return placementWeight;
        }

        @Override
        public void setPlacementWeight(int weight) {
            placementWeight = weight;
        }

        @Override
        public long maxMessageSize() {
            return maxMessageSize;
        }

        @Override
        public void setMaxMessageSize(long value) {
            maxMessageSize = value;
        }

        @Override
        public void setRoutingId(RoutingId routingId) {
        }

        @Override
        public void start() {
        }

        @Override
        public long connectPeer(String endpoint) {
            return 0;
        }

        @Override
        public long connectPeer(
            String endpoint,
            RoutingId expectedRoutingId) {
            return 0;
        }

        @Override
        public MeshNodeStatus status() {
            return null;
        }

        @Override
        public List<MeshPeerEntry> peers() {
            return List.of();
        }

        @Override
        public List<Long> connectionIntentIds() {
            return new ArrayList<>();
        }

        @Override
        public void startDispatch(
            Consumer<ZLinkMeshDispatchRecord> receiver) {
        }

        @Override
        public void close() {
        }
    }
}
