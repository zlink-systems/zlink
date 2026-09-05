package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.lang.reflect.Method;
import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.function.BooleanSupplier;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.eventing.MonitorEvent;
import systems.zlink.contracts.eventing.MonitorEventType;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceAdmissionGuard;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

final class ZLinkJavaRawMeshNodeTransportIdentityTest {
    private static final long INTENT = 2;
    private static final RoutingId PEER = RoutingId.from("transport-identity-peer");
    private static final String ENDPOINT = "inproc://transport-identity-peer";

    @Test
    void foreignTerminationPreservesIntentUntilItsRecordedTransportDisconnects()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            assertTrue(node.hasLivePeerIntent(ENDPOINT));

            // The observed completion-lane termination, then each mismatch
            // independently, must preserve the admitted application transport.
            for (var event : new MonitorEvent[] {
                    event(MonitorEventType.DISCONNECTED, 1439, 1),
                    event(MonitorEventType.DISCONNECTED, 1436, 1),
                    event(MonitorEventType.DISCONNECTED, 1439, 0),
                    event(MonitorEventType.DISCONNECTED, 0, 0)}) {
                terminateAdmitted(node, event);
                assertTrue(node.hasLivePeerIntent(ENDPOINT), event.toString());
                assertFalse(isClosed(node), event.toString());
            }

            terminateAdmitted(node, event(MonitorEventType.DISCONNECTED, 1436, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(isClosed(node));
        }
    }

    @Test
    void intentClosesOnlyAfterEveryRecordedTransportTerminates()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            ready(node, 1439, 1);

            terminateAdmitted(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertTrue(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));
            terminateAdmitted(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertTrue(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));

            terminateAdmitted(node, event(MonitorEventType.CLOSED, 1436, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(isClosed(node));
        }
    }

    @Test
    void closeRequestDoesNotAttributeAnUnrecordedTerminationToIntent()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            requestClose(node);

            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertFalse(isClosed(node));
            assertTrue(node.isPeerConnectionClosing(INTENT));
        }
    }

    @Test
    void readyObservedAfterCloseRequestStillOwnsItsTermination()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            requestClose(node);
            ready(node, 1436, 0);
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(node.isPeerConnectionClosing(INTENT));

            terminate(node, event(MonitorEventType.DISCONNECTED, 1439, 1));
            assertFalse(isClosed(node));
            assertTrue(node.isPeerConnectionClosing(INTENT));

            terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0));
            assertTrue(isClosed(node));
            assertFalse(node.isPeerConnectionClosing(INTENT));
        }
    }

    @Test
    void rejectedAttemptTerminationKeepsIntentUntilAdmittedConnectionCloses()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);

            // Core rejected the first attempt before admission (D-094 retry):
            // neither requested nor admitted, so the intent stays open and
            // only stops being live until Core's retry reaches READY.
            terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));
            assertFalse(node.isPeerConnectionClosing(INTENT));

            ready(node, 1442, 0);
            assertTrue(node.hasLivePeerIntent(ENDPOINT));
            assertFalse(isClosed(node));

            terminateAdmitted(node, event(MonitorEventType.DISCONNECTED, 1442, 0));
            assertFalse(node.hasLivePeerIntent(ENDPOINT));
            assertTrue(isClosed(node));
        }
    }

    @Test
    void lateReadyCannotReviveRequestedClosedIntent() throws Exception {
        assertLateReadyPreservesClosedIntent(false);
    }

    @Test
    void lateReadyCannotReviveAdmittedClosedIntent() throws Exception {
        assertLateReadyPreservesClosedIntent(true);
    }

    private static void assertLateReadyPreservesClosedIntent(boolean admitted)
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            if (!admitted) {
                requestClose(node);
            }
            terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0), admitted);
            assertTrue(isClosed(node));

            Method active = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                "markPeerIntentsActive", MonitorEvent.class, RoutingId.class);
            active.setAccessible(true);
            // Isolate endpoint and RID matching: neither late edge belongs
            // to the intent whose terminal close was already published.
            for (MonitorEvent late : new MonitorEvent[] {
                    new MonitorEvent(MonitorEventType.CONNECTION_READY, 1,
                        Optional.of(RoutingId.from("other-peer")), "", ENDPOINT,
                        1442, 0, 1),
                    new MonitorEvent(MonitorEventType.CONNECTION_READY, 1,
                        Optional.of(PEER), "", "inproc://other-endpoint",
                        1445, 0, 1)}) {
                active.invoke(node, late, late.routingId().orElseThrow());
                assertTrue(isClosed(node), late.toString());
                assertFalse(node.hasLivePeerIntent(ENDPOINT));
                var transportsField = ZLinkJavaRawMeshNode.class
                    .getDeclaredField("peerIntentTransports");
                transportsField.setAccessible(true);
                assertFalse(((Map<?, ?>) transportsField.get(node)).containsKey(INTENT),
                    "a late READY must not become transport ownership of a closed intent");
            }

            var routerField = ZLinkJavaRawMeshNode.class.getDeclaredField("router");
            routerField.setAccessible(true);
            var connects = new java.util.concurrent.atomic.AtomicInteger();
            RouterSocket router = (RouterSocket) Proxy.newProxyInstance(
                RouterSocket.class.getClassLoader(),
                new Class<?>[] {RouterSocket.class},
                (proxy, method, args) -> {
                    assertEquals("connect", method.getName());
                    assertEquals(ENDPOINT, args[0]);
                    connects.incrementAndGet();
                    return null;
                });
            routerField.set(node, router);
            try {
                long replacement = node.replacePeerConnection(ENDPOINT, PEER, 17L,
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
                assertTrue(replacement != INTENT);
                assertEquals(1, connects.get());
                ready(node, 1450, 0);
                assertTrue(node.hasLivePeerIntent(ENDPOINT));
            } finally {
                routerField.set(node, null);
            }
        }
    }

    @Test
    void lateReadyKeepsAdmissionIdentityWhenHelloInferredTheOppositeDirection()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setBind("inproc://late-ready-local-" + System.nanoTime());
            peer.setBind("inproc://late-ready-peer-" + System.nanoTime());
            node.start();
            peer.setRoutingId(PEER);
            peer.start();
            var topologyField = ZLinkJavaRawMeshNode.class.getDeclaredField("topology");
            topologyField.setAccessible(true);
            var topology = (ZLinkServiceTopologyRegistry) topologyField.get(node);
            var peerTopology = (ZLinkServiceTopologyRegistry) topologyField.get(peer);
            String admitted = "wire-admitted-before-ready";
            assertEquals(ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                topology.admit(peerTopology.localDescriptor(),
                    new ZLinkServiceTopologyRegistry.Connection(admitted,
                        ZLinkServiceAdmissionGuard.ConnectionDirection.INBOUND,
                        "hello-inferred-inbound")));
            var connectionsField = ZLinkJavaRawMeshNode.class.getDeclaredField("connectionIds");
            connectionsField.setAccessible(true);
            @SuppressWarnings("unchecked")
            var connections = (Map<RoutingId, String>) connectionsField.get(node);
            connections.put(PEER, admitted);

            Method register = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                "registerTransportConnection", MonitorEvent.class, RoutingId.class);
            register.setAccessible(true);
            assertEquals(admitted, register.invoke(node,
                event(MonitorEventType.CONNECTION_READY, 520, 0), PEER));
        }
    }

    @Test
    void unmappedTerminationPreservesAdmissionCompletion() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            var completedField = ZLinkJavaRawMeshNode.class
                .getDeclaredField("admissionControlReadyConnections");
            completedField.setAccessible(true);
            @SuppressWarnings("unchecked")
            var completed = (Map<RoutingId, String>) completedField.get(node);
            completed.put(PEER, "admitted-connection");

            Method terminal = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                "cleanupTerminalTransport", MonitorEvent.class, RoutingId.class);
            terminal.setAccessible(true);
            assertFalse((boolean) terminal.invoke(node,
                event(MonitorEventType.DISCONNECTED, 182, 0), PEER));
            assertEquals("admitted-connection", completed.get(PEER),
                "an unassociated attempt cannot revoke the admitted connection");
        }
    }

    @Test
    void requestedClosePublishesReplacementEligibilityAfterEndpointRetirement()
        throws Exception {
        assertEndpointRetiredBeforeClosed(false);
    }

    @Test
    void admittedClosePublishesReplacementEligibilityAfterEndpointRetirement()
        throws Exception {
        assertEndpointRetiredBeforeClosed(true);
    }

    private static void assertEndpointRetiredBeforeClosed(boolean admitted)
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            installIntent(node);
            ready(node, 1436, 0);
            if (!admitted) {
                requestClose(node);
            }
            var routerField = ZLinkJavaRawMeshNode.class.getDeclaredField("router");
            routerField.setAccessible(true);
            var retired = new java.util.concurrent.atomic.AtomicBoolean();
            RouterSocket router = (RouterSocket) Proxy.newProxyInstance(
                RouterSocket.class.getClassLoader(),
                new Class<?>[] {RouterSocket.class},
                (proxy, method, args) -> {
                    if (!method.getName().equals("disconnect")) {
                        throw new AssertionError("unexpected router call: " + method);
                    }
                    assertEquals(ENDPOINT, args[0]);
                    // A caller can observe replacement eligibility while the
                    // pump is retiring the endpoint. It must remain fenced
                    // until that operation can no longer close a new intent.
                    assertFalse(isClosed(node),
                        "replacement became eligible before endpoint retirement");
                    retired.set(true);
                    return null;
                });
            routerField.set(node, router);
            try {
                terminate(node, event(MonitorEventType.DISCONNECTED, 1436, 0),
                    admitted);
                assertTrue(retired.get());
                assertTrue(isClosed(node));
                assertFalse(node.isPeerConnectionClosing(INTENT));
            } finally {
                routerField.set(node, null);
            }
        }
    }

    @Test
    void closedIsPublishedExactlyWhenReplacementBecomesEligible() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setBind("inproc://closed-eligibility-local-" + System.nanoTime());
            peer.setBind("inproc://closed-eligibility-peer-" + System.nanoTime());
            node.start();
            peer.setRoutingId(PEER);
            peer.start();
            installIntent(node);
            ready(node, 1436, 0);
            Method register = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                "registerTransportConnection", MonitorEvent.class, RoutingId.class);
            register.setAccessible(true);
            String connection = (String) register.invoke(
                node, event(MonitorEventType.CONNECTION_READY, 1436, 0), PEER);
            var topologyField = ZLinkJavaRawMeshNode.class.getDeclaredField("topology");
            topologyField.setAccessible(true);
            var topology = (ZLinkServiceTopologyRegistry) topologyField.get(node);
            var peerTopology = (ZLinkServiceTopologyRegistry) topologyField.get(peer);
            assertEquals(ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                topology.admit(peerTopology.localDescriptor(),
                    new ZLinkServiceTopologyRegistry.Connection(connection,
                        ZLinkServiceAdmissionGuard.ConnectionDirection.OUTBOUND,
                        "outbound:" + ENDPOINT + ":" + connection)));
            var connectionsField = ZLinkJavaRawMeshNode.class.getDeclaredField("connectionIds");
            connectionsField.setAccessible(true);
            @SuppressWarnings("unchecked")
            var connections = (Map<RoutingId, String>) connectionsField.get(node);
            connections.put(PEER, connection);
            node.admitPeerChannels(PEER, Map.of());
            assertEquals(MeshPeerState.CONNECTING, peerState(node, INTENT));

            // The admitted connection's liveness close, in the order the
            // monitor drain applies it: terminal transport cleanup first,
            // then the intent's closed publication.
            Method terminal = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                "cleanupTerminalTransport", MonitorEvent.class, RoutingId.class);
            terminal.setAccessible(true);
            MonitorEvent disconnected = event(MonitorEventType.DISCONNECTED, 1436, 0);
            boolean admittedClosed = (boolean) terminal.invoke(node, disconnected, PEER);
            assertTrue(admittedClosed);
            // CLOSED is the public form of replacement eligibility (mesh-node
            // §7.1 (3)); a caller that acts on it must not be rejected.
            assertEquals(isClosed(node), peerState(node, INTENT) == MeshPeerState.CLOSED,
                "CLOSED published before replacement eligibility");
            assertEquals(MeshPeerState.CONNECTING, peerState(node, INTENT));

            terminate(node, disconnected, admittedClosed);
            assertTrue(isClosed(node));
            assertEquals(MeshPeerState.CLOSED, peerState(node, INTENT));
        }
    }

    @Test
    void observedInprocCloseRetiresTheCoreConnectIntent() throws Exception {
        RoutingId localRid = RoutingId.from("jvm-retire-intent-local");
        RoutingId peerRid = RoutingId.from("jvm-retire-intent-peer");
        String localEndpoint = "inproc://jvm-retire-intent-local-"
            + System.nanoTime();
        String peerEndpoint = "inproc://jvm-retire-intent-peer-"
            + System.nanoTime();
        try (var context = Zlink.createContext();
             var local = new ZLinkJavaRawMeshNode(context, "mesh");
             var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            local.setRoutingId(localRid);
            local.setBind(localEndpoint);
            peer.setRoutingId(peerRid);
            peer.setBind(peerEndpoint);
            local.start();
            peer.start();

            long intent = local.connectPeer(peerEndpoint, peerRid);
            assertEquals(intent, awaitState(local, MeshPeerState.ADMITTED)
                .connectionIntentId());
            peer.close();
            awaitState(local, MeshPeerState.CLOSED);

            // A fresh listener on the same endpoint must not revive the
            // closed intent: its Core connect intent was retired with it,
            // so the only connection to the replacement is the one the
            // replacement intent creates.
            try (var replacementPeer = new ZLinkJavaRawMeshNode(
                     context, "mesh")) {
                replacementPeer.setRoutingId(peerRid);
                replacementPeer.setBind(peerEndpoint);
                replacementPeer.start();
                long revived = System.nanoTime()
                    + Duration.ofMillis(200).toNanos();
                while (System.nanoTime() < revived) {
                    assertFalse(local.hasLivePeerIntent(peerEndpoint));
                    assertEquals(
                        MeshPeerState.CLOSED,
                        peerState(local, intent));
                    Thread.sleep(5);
                }

                long replacement = local.replacePeerConnection(
                    peerEndpoint,
                    peerRid,
                    replacementPeer.status().lifecycleGeneration(),
                    ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
                assertEquals(
                    replacement,
                    awaitState(local, MeshPeerState.ADMITTED)
                        .connectionIntentId());
                await(() -> local.hasLivePeerIntent(peerEndpoint));
            }
        }
    }

    private static MeshPeerState peerState(
        ZLinkJavaRawMeshNode node,
        long intent) {
        return node.peers().stream()
            .filter(entry -> entry.connectionIntentId() == intent)
            .map(MeshPeerEntry::state)
            .findFirst()
            .orElseThrow();
    }

    private static MeshPeerEntry awaitState(
        ZLinkJavaRawMeshNode node,
        MeshPeerState state) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            Optional<MeshPeerEntry> matched = node.peers().stream()
                .filter(entry -> entry.state() == state)
                .findFirst();
            if (matched.isPresent()) {
                return matched.orElseThrow();
            }
            Thread.sleep(1);
        }
        throw new AssertionError("peer state was not observed: " + state);
    }

    private static void await(BooleanSupplier condition)
        throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (!condition.getAsBoolean()) {
            if (System.nanoTime() >= deadline) {
                throw new AssertionError("condition was not observed");
            }
            Thread.sleep(1);
        }
    }

    private static void requestClose(ZLinkJavaRawMeshNode node) throws Exception {
        var closingField = ZLinkJavaRawMeshNode.class
            .getDeclaredField("closeRequestedPeerIntents");
        closingField.setAccessible(true);
        @SuppressWarnings("unchecked")
        var closing = (Set<Long>) closingField.get(node);
        closing.add(INTENT);
    }

    private static void installIntent(ZLinkJavaRawMeshNode node)
        throws Exception {
        // Seed only the configured intent; drive the real monitor handlers
        // synchronously so native scheduling cannot hide foreign attribution.
        var intentClass = Class.forName(
            ZLinkJavaRawMeshNode.class.getName() + "$PeerIntent");
        var constructor = intentClass.getDeclaredConstructor(
            String.class, RoutingId.class, long.class, String.class, long.class);
        constructor.setAccessible(true);
        var intentsField = ZLinkJavaRawMeshNode.class.getDeclaredField("peerIntents");
        intentsField.setAccessible(true);
        @SuppressWarnings("unchecked")
        var intents = (Map<Long, Object>) intentsField.get(node);
        intents.put(INTENT, constructor.newInstance(ENDPOINT, PEER, 0L, null, 0L));
    }

    private static MonitorEvent event(
        MonitorEventType type, long connectionId, int lane) {
        return new MonitorEvent(
            type, type == MonitorEventType.CONNECTION_READY ? 1 : 4,
            Optional.of(PEER), "inproc://transport-identity-local", ENDPOINT,
            connectionId, lane, type == MonitorEventType.CONNECTION_READY ? 1 : 0);
    }

    private static void ready(ZLinkJavaRawMeshNode node, long id, int lane)
        throws Exception {
        Method active = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "markPeerIntentsActive", MonitorEvent.class, RoutingId.class);
        active.setAccessible(true);
        active.invoke(node, event(MonitorEventType.CONNECTION_READY, id, lane), PEER);
    }

    // A termination that did not close the peer's admitted connection
    // (a pre-admission attempt, or a foreign transport).
    private static void terminate(ZLinkJavaRawMeshNode node, MonitorEvent event)
        throws Exception {
        terminate(node, event, false);
    }

    // A termination the monitor path attributed to the peer's admitted
    // connection (its liveness close).
    private static void terminateAdmitted(
        ZLinkJavaRawMeshNode node,
        MonitorEvent event) throws Exception {
        terminate(node, event, true);
    }

    private static void terminate(
        ZLinkJavaRawMeshNode node,
        MonitorEvent event,
        boolean admittedConnectionClosed) throws Exception {
        Method terminal = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "markPeerIntentsClosed", MonitorEvent.class, boolean.class);
        terminal.setAccessible(true);
        terminal.invoke(node, event, admittedConnectionClosed);
    }

    private static boolean isClosed(ZLinkJavaRawMeshNode node) throws Exception {
        Method closed = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
            "peerIntentIsClosed", long.class);
        closed.setAccessible(true);
        return (boolean) closed.invoke(node, INTENT);
    }
}
