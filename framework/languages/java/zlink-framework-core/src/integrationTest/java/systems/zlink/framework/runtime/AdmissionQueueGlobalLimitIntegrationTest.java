package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkListenerKind;
import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.stream.connector.ZLinkStreamConnector;
import systems.zlink.stream.connector.ZLinkStreamConnectorFactory;
import systems.zlink.stream.connector.ZLinkStreamConnectorOptions;

import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/**
 * Two real nodes and the queue Instance Spot: with AdmissionGlobalLimit 1, three clients on two
 * nodes never run more than one authentication handler at a time and are all admitted
 * (Authentication Admission §3.5.3).
 */
final class AdmissionQueueGlobalLimitIntegrationTest {
    private static final String KEY = "AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8";
    private static final AtomicInteger running = new AtomicInteger();
    private static final AtomicInteger peak = new AtomicInteger();
    private static final AtomicInteger completed = new AtomicInteger();

    private record Login(String user) {}

    @Test
    void globalLimitOneAdmitsThreeClientsOfTwoNodesOneAtATime() throws Exception {
        Zlink.version();
        running.set(0);
        peak.set(0);
        completed.set(0);
        String suffix = Long.toUnsignedString(System.nanoTime(), 36);
        var store = new ZLinkInMemoryLocationStore();
        var optionsA = nodeOptions(store, "queue-a-" + suffix, true);
        var optionsB = nodeOptions(store, "queue-b-" + suffix, false);

        try (ZLinkFrameworkRuntime nodeA =
                        RuntimeTestSupport.startFixedRidFramework(
                                optionsA, new ZLinkJavaBackendAdapterFactory());
                ZLinkFrameworkRuntime nodeB =
                        RuntimeTestSupport.startFixedRidFramework(
                                optionsB, new ZLinkJavaBackendAdapterFactory())) {
            awaitPeers(nodeA, "queue-b-" + suffix);
            awaitPeers(nodeB, "queue-a-" + suffix);
            List<ZLinkStreamConnector> connectors = new ArrayList<>();
            List<CompletableFuture<String>> replies = new ArrayList<>();
            for (var node : List.of(nodeA, nodeA, nodeB)) {
                var endpoint =
                        URI.create(
                                node.listenerStatus(
                                                ZLinkListenerKind.STREAM,
                                                node == nodeA ? "login-a" : "login-b")
                                        .endpoint());
                var connector =
                        ZLinkStreamConnectorFactory.create(
                                ZLinkStreamConnectorOptions.createDefault(endpoint));
                connectors.add(connector);
                connector.connect().submit().toCompletableFuture().get(10, TimeUnit.SECONDS);
                replies.add(
                        connector
                                .request(new Login("user-" + connectors.size()))
                                .timeout(Duration.ofSeconds(90))
                                .submit(String.class)
                                .toCompletableFuture());
            }
            for (var reply : replies) {
                assertEquals("ok", reply.get(100, TimeUnit.SECONDS));
            }
            assertEquals(3, completed.get());
            assertTrue(peak.get() <= 1, "handlers overlapped: peak=" + peak.get());
            for (var connector : connectors) {
                connector.close();
            }
        }
    }

    private static DefaultZLinkFrameworkOptions nodeOptions(
            ZLinkInMemoryLocationStore store, String rid, boolean first) {
        var options = new DefaultZLinkFrameworkOptions();
        options.addLocationStore(store);
        options.addRelocationStore(new InMemoryRelocationStore());
        options.configureLocations().setPollingInterval(Duration.ofMillis(20));
        options.addRouteMesh("game")
                .listen("tcp://127.0.0.1:0")
                .setRoutingId(RoutingId.from(rid))
                .objects()
                .server();
        options.addStreamNode(first ? "login-a" : "login-b")
                .bind("tcp://127.0.0.1:0")
                .registerSession(first ? SessionA.class : SessionB.class)
                .authenticationPacketName("Login")
                .admissionQueueEnabled(true)
                .admissionQueueKey(KEY)
                .admissionGlobalLimit(1);
        return options;
    }

    private static void awaitPeers(ZLinkFrameworkRuntime runtime, String peerRid) throws Exception {
        RoutingId rid = RoutingId.from(peerRid);
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(10);
        while (runtime.routeMeshRuntime().snapshot("game").peers().stream()
                .noneMatch(
                        peer ->
                                peer.nodeRid().equals(rid)
                                        && peer.state()
                                                == systems.zlink.framework.monitoring.ZLinkPeerState
                                                        .READY)) {
            if (System.nanoTime() > deadline) {
                throw new java.util.concurrent.TimeoutException("peer never became ready");
            }
            Thread.sleep(5);
        }
    }

    private abstract static class BaseSession implements ZLinkSession {
        private final ZLinkSessionContext context;

        BaseSession(ZLinkSessionContext context) {
            this.context = context;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
                ZLinkSessionDispatchContext dispatch, ZLinkMessage message) {
            int now = running.incrementAndGet();
            peak.accumulateAndGet(now, Math::max);
            Executor later = CompletableFuture.delayedExecutor(400, TimeUnit.MILLISECONDS);
            return CompletableFuture.runAsync(() -> {}, later)
                    .thenCompose(
                            ignored -> {
                                running.decrementAndGet();
                                completed.incrementAndGet();
                                return context().client().reply("ok").submit();
                            });
        }
    }

    public static final class SessionA extends BaseSession {
        public SessionA(ZLinkSessionContext context) {
            super(context);
        }
    }

    public static final class SessionB extends BaseSession {
        public SessionB(ZLinkSessionContext context) {
            super(context);
        }
    }
}
