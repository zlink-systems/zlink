package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.CsvSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;

final class ZLinkJavaRawMeshNodeHelloRetryTest {
    private static final Duration IO_DEADLINE = Duration.ofSeconds(2);

    @ParameterizedTest
    @CsvSource({"false,false", "true,false", "false,true", "true,true"})
    void eachLivenessTurnResendsHelloUntilAdmitIncludingInboundRoutes(
            boolean withIntent, boolean pausePublication) throws Exception {
        var localRid = RoutingId.from("hello-retry-local");
        var remoteRid = RoutingId.from("hello-retry-remote");
        var wire = new ZLinkServiceM6AWireCodec();
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "hello-retry");
                var peerPort = new ZLinkJavaRawServicePort(context);
                var peer = peerPort.openRouter(remoteRid)) {
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            var router = port.openRouter(localRid);
            String endpoint = "inproc://hello-retry-" + java.util.UUID.randomUUID();
            router.bind(endpoint);
            set(node, "router", router);
            set(node, "localDescriptor", descriptor(localRid));
            set(node, "topology", new ZLinkServiceTopologyRegistry(descriptor(localRid)));
            if (withIntent) {
                String peerEndpoint = descriptor(remoteRid).advertisedEndpoint();
                peer.bind(peerEndpoint);
                node.connectPeer(peerEndpoint, remoteRid);
            } else {
                peer.options().probe(true);
                peer.connect(endpoint);
            }
            port.ensureReceivePollerRegistered(router);
            long deadline = System.nanoTime() + IO_DEADLINE.toNanos();
            while (port.routesSnapshot(router).isEmpty()) {
                assertTrue(System.nanoTime() < deadline, "Core route did not become selectable");
                port.waitForReady(router, IO_DEADLINE);
            }
            invoke(node, "observeSelectedRoutes");
            if (pausePublication) {
                var lane =
                        (systems.zlink.framework.runtime.internal.execution.ZLinkStateLane)
                                field(node, "descriptorStateLane");
                var entered = new java.util.concurrent.CompletableFuture<Void>();
                var release = new java.util.concurrent.CompletableFuture<Void>();
                lane.runAsync(
                        () -> {
                            entered.complete(null);
                            release.join();
                            return null;
                        });
                try {
                    entered.get(2, java.util.concurrent.TimeUnit.SECONDS);
                    tick(node, 0);
                    var queued = node.helloExchanges.get(remoteRid).submission();
                    tick(node, 1);
                    release.complete(null);
                    queued.get(2, java.util.concurrent.TimeUnit.SECONDS);
                    lane.runAsync(() -> null)
                            .toCompletableFuture()
                            .get(2, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));
                    port.send(
                                    router,
                                    remoteRid,
                                    List.of(
                                            wire.encodeAdmission(
                                                    ServiceWireConstants.COMMAND_UPDATE,
                                                    descriptor(localRid))))
                            .toCompletableFuture()
                            .get(2, java.util.concurrent.TimeUnit.SECONDS);
                    assertEquals(
                            ServiceWireConstants.COMMAND_UPDATE,
                            receive(peerPort, peer, wire),
                            "a queued HELLO must not be submitted twice before its completion");
                } finally {
                    release.complete(null);
                }
            }
            // No receive pump or wall-clock retry runs. These are explicit management turns.
            tick(node, 0);
            var first = node.helloExchanges.get(remoteRid);
            if (first != null && first.submission() != null)
                first.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
            assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));
            tick(node, 1);
            var second = node.helloExchanges.get(remoteRid);
            if (second != null && second.submission() != null)
                second.submission().get(2, java.util.concurrent.TimeUnit.SECONDS);
            assertEquals(ServiceWireConstants.COMMAND_HELLO, receive(peerPort, peer, wire));

            peerPort.send(
                            peer,
                            localRid,
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_ADMIT,
                                            descriptor(remoteRid))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            ZLinkJavaRawServicePort.Inbound admit;
            do {
                assertTrue(port.waitForReadable(router, IO_DEADLINE));
                admit = port.receive(router).orElseThrow();
                if (admit.frames().getFirst().length != 0) break;
                admit.close();
            } while (true);
            var dispatch =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "dispatch", ZLinkJavaRawServicePort.Inbound.class);
            dispatch.setAccessible(true);
            dispatch.invoke(node, admit);
            tick(node, System.nanoTime());
            // ADMIT sends UPDATE and the next management turn sends a liveness probe.
            // That FIFO marker proves the turn sent no further HELLO, without a timed absence test.
            int command;
            do {
                command = receive(peerPort, peer, wire);
                assertNotEquals(ServiceWireConstants.COMMAND_HELLO, command);
            } while (command != ServiceWireConstants.COMMAND_LIVENESS_PROBE);
        }
    }

    private static int receive(
            ZLinkJavaRawServicePort port, RouterSocket router, ZLinkServiceM6AWireCodec wire) {
        while (true) {
            assertTrue(port.waitForReadable(router, IO_DEADLINE), "expected protocol record");
            try (var inbound = port.receive(router).orElseThrow()) {
                if (inbound.frames().getFirst().length == 0) continue;
                return wire.decodeHeader(inbound.frames().getFirst()).command();
            }
        }
    }

    private static void tick(ZLinkJavaRawMeshNode node, long now) throws Exception {
        var method = ZLinkJavaRawMeshNode.class.getDeclaredMethod("tickLiveness", long.class);
        method.setAccessible(true);
        method.invoke(node, now);
    }

    private static void invoke(Object owner, String name) throws Exception {
        var method = owner.getClass().getDeclaredMethod(name);
        method.setAccessible(true);
        method.invoke(owner);
    }

    private static Object field(Object owner, String name) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void set(Object owner, String name, Object value) throws Exception {
        var field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        field.set(owner, value);
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid) {
        return new ZLinkServiceNodeDescriptor(
                "hello-retry",
                rid,
                1,
                1,
                "inproc://" + rid,
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                1,
                1,
                0,
                0,
                0);
    }
}
