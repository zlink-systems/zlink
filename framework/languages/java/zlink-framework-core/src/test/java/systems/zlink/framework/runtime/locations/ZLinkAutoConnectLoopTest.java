package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkLocationRole;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeer;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectPeerResolver;
import systems.zlink.framework.runtime.internal.locations.ZLinkAutoConnectType;

import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;

final class ZLinkAutoConnectLoopTest {
    @Test
    void startupUsesConfiguredPollingInterval() throws Exception {
        PendingResolver resolver = new PendingResolver();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        options.setPollingInterval(java.time.Duration.ofSeconds(30));
        ZLinkAutoConnectReconciler reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        new RecordingExecutor(),
                        options);
        ZLinkAutoConnectLoop loop = new ZLinkAutoConnectLoop(reconciler, options);
        try {
            loop.start();
            resolver.complete(List.of());
            var taskField = ZLinkAutoConnectLoop.class.getDeclaredField("task");
            taskField.setAccessible(true);
            var scheduled = (java.util.concurrent.ScheduledFuture<?>) taskField.get(loop);
            assertTrue(
                    scheduled.getDelay(TimeUnit.SECONDS) > 20,
                    "startup must use the configured polling interval");
        } finally {
            loop.stop().toCompletableFuture().get(1, TimeUnit.SECONDS);
        }
    }

    @Test
    void stopWaitsForInFlightTickBeforeDiscardingReconcilerState() throws Exception {
        PendingResolver resolver = new PendingResolver();
        RecordingExecutor executor = new RecordingExecutor();
        ZLinkLocationOptions options = new ZLinkLocationOptions();
        ZLinkAutoConnectReconciler reconciler =
                new ZLinkAutoConnectReconciler(
                        new ZLinkAutoConnectPlanner.Local(
                                ZLinkAutoConnectType.CLIENT_SERVER,
                                "orders",
                                ZLinkLocationRole.DEALER,
                                RoutingId.from("client"),
                                "inproc://client"),
                        resolver,
                        executor,
                        options);
        ZLinkAutoConnectLoop loop = new ZLinkAutoConnectLoop(reconciler, options);

        CompletionStage<Void> tick = loop.start();
        CompletableFuture<Void> stop = loop.stop().toCompletableFuture();

        assertFalse(
                stop.isDone(),
                "stop must join the in-flight tick before clearing reconciler state");
        resolver.complete(List.of(peer()));
        stop.get(1, TimeUnit.SECONDS);

        assertEquals(List.of("connect", "disconnect"), executor.operations);
        assertTrue(tick.toCompletableFuture().isDone());
    }

    private static ZLinkAutoConnectPeer peer() {
        return new ZLinkAutoConnectPeer(
                ZLinkAutoConnectType.CLIENT_SERVER,
                "orders",
                RoutingId.from("server"),
                ZLinkLocationRole.ROUTER,
                "inproc://server",
                100,
                false,
                9,
                Map.of(),
                List.of(),
                "owner-server",
                4,
                Instant.parse("2026-07-27T00:00:00Z"));
    }

    private static final class PendingResolver implements ZLinkAutoConnectPeerResolver {
        private final CompletableFuture<List<ZLinkAutoConnectPeer>> result =
                new CompletableFuture<>();

        @Override
        public CompletionStage<List<ZLinkAutoConnectPeer>> listPeers(
                ZLinkAutoConnectType type, String meshName, ZLinkLocationRole role) {
            return result;
        }

        void complete(List<ZLinkAutoConnectPeer> peers) {
            result.complete(peers);
        }
    }

    private static final class RecordingExecutor implements ZLinkAutoConnectExecutor {
        private final List<String> operations = new ArrayList<>();

        @Override
        public boolean connect(ZLinkAutoConnectPlanner.Target target) {
            operations.add("connect");
            return true;
        }

        @Override
        public boolean disconnect(ZLinkAutoConnectPlanner.Target target) {
            operations.add("disconnect");
            return true;
        }
    }
}
