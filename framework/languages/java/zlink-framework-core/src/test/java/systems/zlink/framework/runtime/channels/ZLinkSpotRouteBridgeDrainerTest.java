package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

final class ZLinkSpotRouteBridgeDrainerTest {
    @Test
    void drainsOnlyTheReadableChannelAndThenDispatches() {
        List<String> order = new ArrayList<>();
        var first = new RecordingBridge("first", order);
        var second = new RecordingBridge("second", order);
        var drainer = new ZLinkSpotRouteBridgeDrainer(Map.of("a", first, "z", second));
        drainer.setDispatchDrainer(() -> order.add("dispatch"));
        drainer.drainNow("z");
        assertEquals(List.of("second", "dispatch"), order);
        drainer.drainNow("a");
        assertEquals(List.of("second", "dispatch", "first", "dispatch"), order);
        drainer.drainNow("absent");
        assertEquals(4, order.size());
    }

    private static final class RecordingBridge implements ZLinkBackendSpotRouteBridge {
        private final String name;
        private final List<String> order;

        private RecordingBridge(String name, List<String> order) {
            this.name = name;
            this.order = order;
        }

        @Override
        public void attachRouterChannel(String channelName, ZLinkBackendRouterSocket router) {}

        @Override
        public CompletionStage<Void> send(
                String channelName,
                RoutingId targetNodeRid,
                String targetSpotId,
                List<Message> parts) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<List<Message>> request(
                String channelName,
                RoutingId targetNodeRid,
                String targetSpotId,
                List<Message> parts,
                Duration timeout) {
            return CompletableFuture.completedFuture(List.of());
        }

        @Override
        public boolean handleRouterReceived(
                String channelName, RoutingId sourceNodeRid, long requestSeq, List<Message> parts) {
            return false;
        }

        @Override
        public int drain() {
            order.add(name);
            return 0;
        }

        @Override
        public String name() {
            return name;
        }

        @Override
        public void close() {}
    }
}
