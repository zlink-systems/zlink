package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;

import java.lang.reflect.Method;
import java.time.Duration;
import java.util.Collection;
import java.util.HashMap;
import java.util.Set;
import java.util.concurrent.AbstractExecutorService;
import java.util.concurrent.TimeUnit;

final class ZLinkJavaRawMeshNodeManagementWaitTest {
    @Test
    void tickReturnsTheNextManagementDeadline() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "management-wait")) {
            var field = ZLinkJavaRawMeshNode.class.getDeclaredField("liveness");
            field.setAccessible(true);
            var liveness = (ZLinkServiceLivenessRegistry) field.get(node);
            liveness.admit(RoutingId.from("deadline-peer"), "connection", 0);
            var tick = liveness.tick(0);
            assertEquals(
                    ZLinkServiceLivenessRegistry.DEFAULT_PROBE_INTERVAL, tick.nextWaitTimeout());
        }
    }

    @Test
    void expiryBeforeTheNextProbeOwnsTheWaitAndSubMillisecondWaitsStayBlocking() {
        var liveness = new ZLinkServiceLivenessRegistry();
        liveness.admit(RoutingId.from("expiry-peer"), "connection", 0);
        var beforeExpiry = liveness.tick(Duration.ofSeconds(14).toNanos());
        assertEquals(Duration.ofSeconds(1), beforeExpiry.nextWaitTimeout());
        var beforeExpirySubMillisecond = liveness.tick(Duration.ofSeconds(15).toNanos() - 1);
        assertEquals(Duration.ofMillis(1), beforeExpirySubMillisecond.nextWaitTimeout());
        var expired = liveness.tick(Duration.ofSeconds(15).toNanos());
        assertEquals(
                ZLinkServiceLivenessRegistry.NO_MANAGEMENT_DEADLINE, expired.nextWaitTimeout());
    }

    @Test
    void elapsedDeadlineUsesZeroWait() throws Exception {
        Method toWaitTimeout =
                ZLinkServiceLivenessRegistry.class.getDeclaredMethod(
                        "toWaitTimeout", long.class, long.class);
        toWaitTimeout.setAccessible(true);
        assertEquals(
                Duration.ZERO,
                toWaitTimeout.invoke(
                        null, Duration.ofSeconds(15).toNanos(), Duration.ofSeconds(15).toNanos()));
    }

    @Test
    void noManagementDeadlineDoesNotSchedulePeriodicDataChecks() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "no-deadline")) {
            assertEquals(
                    ZLinkServiceLivenessRegistry.NO_MANAGEMENT_DEADLINE,
                    liveness(node).tick(0).nextWaitTimeout());
        }
    }

    @Test
    void pumpTurnVisitsEachLivenessPeerOnce() throws Exception {
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "single-liveness-scan")) {
            var liveness = liveness(node);
            var peers = new CountingPeerMap();
            setField(liveness, "peers", peers);
            var peer = RoutingId.from("expired-peer");
            long expiredAt =
                    System.nanoTime()
                            - ZLinkServiceLivenessRegistry.DEFAULT_PEER_TIMEOUT.toNanos()
                            - 1;
            liveness.admit(peer, "connection", expiredAt);

            var port = (ZLinkJavaRawServicePort) field(node, "port").get(node);
            var router = port.openRouter(RoutingId.from("single-scan-node"), true);
            field(node, "router").set(node, router);
            field(node, "pump").set(node, new NoOpExecutorService());

            Method pumpTurn =
                    java.util.Arrays.stream(ZLinkJavaRawMeshNode.class.getDeclaredMethods())
                            .filter(method -> method.getName().equals("pumpTurn"))
                            .findFirst()
                            .orElseThrow();
            pumpTurn.setAccessible(true);
            pumpTurn.invoke(node, router, Duration.ZERO);

            assertEquals(1, peers.visits());
        }
    }

    private static ZLinkServiceLivenessRegistry liveness(ZLinkJavaRawMeshNode node)
            throws Exception {
        return (ZLinkServiceLivenessRegistry) field(node, "liveness").get(node);
    }

    private static java.lang.reflect.Field field(Object target, String name) throws Exception {
        var field = target.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field;
    }

    private static void setField(Object target, String name, Object value) throws Exception {
        field(target, name).set(target, value);
    }

    private static final class CountingPeerMap
            extends HashMap<RoutingId, ZLinkServiceLivenessRegistry.PeerState> {
        private int visits;

        int visits() {
            return visits;
        }

        @Override
        public Set<Entry<RoutingId, ZLinkServiceLivenessRegistry.PeerState>> entrySet() {
            visits++;
            return super.entrySet();
        }

        @Override
        public Collection<ZLinkServiceLivenessRegistry.PeerState> values() {
            visits++;
            return super.values();
        }
    }

    private static final class NoOpExecutorService extends AbstractExecutorService {
        private boolean shutdown;

        @Override
        public void shutdown() {
            shutdown = true;
        }

        @Override
        public java.util.List<Runnable> shutdownNow() {
            shutdown = true;
            return java.util.List.of();
        }

        @Override
        public boolean isShutdown() {
            return shutdown;
        }

        @Override
        public boolean isTerminated() {
            return shutdown;
        }

        @Override
        public boolean awaitTermination(long timeout, TimeUnit unit) {
            return shutdown;
        }

        @Override
        public void execute(Runnable command) {}
    }
}
