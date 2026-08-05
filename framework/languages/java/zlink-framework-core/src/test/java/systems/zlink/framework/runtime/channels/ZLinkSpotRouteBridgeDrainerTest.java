package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.Collections;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;

final class ZLinkSpotRouteBridgeDrainerTest {
    @Test
    void rotatesSortedBridgeChannelsAcrossDrainTicks() throws Exception {
        List<String> order = Collections.synchronizedList(new ArrayList<>());
        CountDownLatch fourDrains = new CountDownLatch(4);
        var first = new RecordingBridge("first", order, fourDrains);
        var second = new RecordingBridge("second", order, fourDrains);
        Map<String, ZLinkBackendSpotRouteBridge> bridges = new HashMap<>();
        bridges.put("z-channel", second);
        bridges.put("a-channel", first);
        ScheduledExecutorService scheduler =
            Executors.newSingleThreadScheduledExecutor();
        try {
            new ZLinkSpotRouteBridgeDrainer(
                bridges,
                scheduler,
                () -> true,
                (channel, failure) -> { throw new AssertionError(failure); })
                .start();

            assertTrue(fourDrains.await(1, TimeUnit.SECONDS));
            scheduler.shutdownNow();
            assertEquals(
                List.of("first", "second", "second", "first"),
                List.copyOf(order.subList(0, 4)));
        } finally {
            scheduler.shutdownNow();
        }
    }

    private static final class RecordingBridge
        implements ZLinkBackendSpotRouteBridge {
        private final String name;
        private final List<String> order;
        private final CountDownLatch drainCount;

        private RecordingBridge(
            String name,
            List<String> order,
            CountDownLatch drainCount) {
            this.name = name;
            this.order = order;
            this.drainCount = drainCount;
        }

        @Override
        public void attachRouterChannel(
            String channelName,
            ZLinkBackendRouterSocket router) {
        }

        @Override
        public boolean send(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            SendFlags flags) {
            return true;
        }

        @Override
        public boolean request(
            String channelName,
            RoutingId targetNodeRid,
            String targetSpotId,
            List<Message> parts,
            ZLinkBackendRequestCallback callback,
            SendFlags flags,
            Duration timeout) {
            return true;
        }

        @Override
        public boolean handleRouterReceived(
            String channelName,
            RoutingId sourceNodeRid,
            long requestSeq,
            List<Message> parts) {
            return false;
        }

        @Override
        public int drain() {
            order.add(name);
            drainCount.countDown();
            return 0;
        }

        @Override
        public String name() {
            return name;
        }

        @Override
        public void close() {
        }
    }
}
