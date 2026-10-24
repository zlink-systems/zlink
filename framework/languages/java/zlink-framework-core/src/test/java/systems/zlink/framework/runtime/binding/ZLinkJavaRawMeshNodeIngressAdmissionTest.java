package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Received;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.util.List;

final class ZLinkJavaRawMeshNodeIngressAdmissionTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void updateCannotEstablishAdmissionOnAReplacementRoute(boolean admittedRoute) throws Exception {
        var rid = RoutingId.from("ingress-peer");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "ingress")) {
            var local = descriptor(RoutingId.from("ingress-local"), 1);
            var topology = new ZLinkServiceTopologyRegistry(local);
            // A synthetic Received has route generation zero. The other connection is
            // still admitted in topology while Core delivers a replacement route's record.
            topology.admit(
                    descriptor(rid, 1),
                    admittedRoute ? "0" : "1",
                    ServiceWireConstants.COMMAND_HELLO);
            set(node, "localDescriptor", local);
            set(node, "topology", topology);
            var dispatch =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                            "dispatch", ZLinkJavaRawServicePort.Inbound.class);
            dispatch.setAccessible(true);
            var update =
                    new ZLinkServiceM6AWireCodec()
                            .encodeAdmission(
                                    ServiceWireConstants.COMMAND_UPDATE, descriptor(rid, 2));
            dispatch.invoke(
                    node,
                    new ZLinkJavaRawServicePort.Inbound(
                            rid, null, List.of(update), new Received()));
            assertEquals(
                    admittedRoute ? 2 : 1,
                    topology.peer(rid).orElseThrow().descriptor().descriptorRevision());
            assertEquals(
                    admittedRoute ? "0" : "1", topology.peer(rid).orElseThrow().connectionId());
        }
    }

    @ParameterizedTest
    @ValueSource(ints = {ServiceWireConstants.COMMAND_HELLO, ServiceWireConstants.COMMAND_ADMIT})
    void endedRouteHandshakeCannotRestoreAdmission(int command) throws Exception {
        var rid = RoutingId.from("ingress-peer");
        try (var context = Zlink.createContext();
                var node = new ZLinkJavaRawMeshNode(context, "ingress");
                var remote = new ZLinkJavaRawServicePort(context)) {
            var local = descriptor(RoutingId.from("ingress-local"), 1);
            var topology = new ZLinkServiceTopologyRegistry(local);
            set(node, "localDescriptor", local);
            set(node, "topology", topology);
            var port = (ZLinkJavaRawServicePort) field(node, "port");
            var router = port.openRouter(local.nodeRoutingId(), true);
            set(node, "router", router);
            var endpoint = "inproc://ended-admission-" + java.util.UUID.randomUUID();
            router.bind(endpoint);
            var peer = remote.openRouter(rid);
            peer.connect(endpoint);
            remote.send(
                            peer,
                            local.nodeRoutingId(),
                            List.of(
                                    new ZLinkServiceM6AWireCodec()
                                            .encodeAdmission(command, descriptor(rid, 1))))
                    .toCompletableFuture()
                    .get(2, java.util.concurrent.TimeUnit.SECONDS);
            ZLinkJavaRawServicePort.Inbound inbound;
            do {
                assertTrue(port.waitForReadable(router, java.time.Duration.ofSeconds(2)));
                inbound = port.receiveNow(router);
                assertNotNull(inbound);
                if (inbound.frames().getFirst().length != 0) break;
                inbound.close();
            } while (true);
            try (var retained = inbound) {
                var observe = ZLinkJavaRawMeshNode.class.getDeclaredMethod("observeSelectedRoutes");
                observe.setAccessible(true);
                observe.invoke(node);
                peer.close();
                long deadline = System.nanoTime() + java.time.Duration.ofSeconds(2).toNanos();
                do {
                    assertTrue(System.nanoTime() < deadline, "selected route must end");
                    port.waitForReady(router, java.time.Duration.ofMillis(10));
                    observe.invoke(node);
                } while (!((java.util.Map<?, ?>) field(node, "selectedRoutes")).isEmpty());
                // Core ROUTER §10.1 permits buffered records after a selected pipe ends.
                // Receipt is not selection, so a pending handshake cannot restore admission.
                var dispatch =
                        ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                                "dispatch", ZLinkJavaRawServicePort.Inbound.class);
                dispatch.setAccessible(true);
                dispatch.invoke(node, retained);
                assertTrue(topology.peer(rid).isEmpty());
                assertNull(
                        ((systems.zlink.framework.runtime.internal.service
                                                .ZLinkServiceLivenessRegistry)
                                        field(node, "liveness"))
                                .connection(rid));
                assertFalse(node.helloExchanges.containsKey(rid));
            }
        }
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

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long revision) {
        return new ZLinkServiceNodeDescriptor(
                "ingress",
                rid,
                1,
                revision,
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
