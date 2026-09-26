package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * RouteMesh admission follows the Core selected route (#1087 FW01). Core ROUTER §10.1 owns which
 * pipe carries a RID; the framework observes the selected-route snapshot and admits only that route
 * (server 05-transport-liveness §5). A replaced route loses its admission and the new route admits
 * through a new handshake, even when Core keeps the old pipe as a standby and never reports it
 * disconnected.
 */
final class ZLinkJavaRawMeshNodeSelectedRouteTest {
    private static final RoutingId TARGET = RoutingId.from("jvm-selected-route-target");

    @Test
    void admissionFollowsCoreSelectedRouteAcrossHandoverAndPromotion() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var first = new ZLinkJavaRawMeshNode(context, "mesh");
                var second = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, RoutingId.from("jvm-selected-route-source"));
            start(first, TARGET);
            start(second, TARGET);

            // Endpoint-only manual intent: the admitted identity is whatever
            // process answers the handshake on the Core selected route, and
            // the intent learns the RID from that handshake.
            source.connectPeer(endpoint(first));
            await(() -> admittedGeneration(source) == first.lifecycleGeneration());
            assertTrue(source.isPeerTransportConnected(TARGET));

            // Same RID, new process: Core hands the selected route to the new
            // pipe and keeps the first pipe as a standby without a disconnect.
            source.connectPeer(endpoint(second));
            await(() -> admittedGeneration(source) == second.lifecycleGeneration());

            // The selected pipe ends; Core promotes the standby to a new route
            // generation. The first process answers the new handshake on it.
            second.close();
            await(() -> admittedGeneration(source) == first.lifecycleGeneration());
            assertTrue(source.isPeerTransportConnected(TARGET));
        }
    }

    @Test
    void routeLossKeepsTheIntentAndARequestedCloseCompletesWithoutARoute() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(source, RoutingId.from("jvm-selected-route-close-source"));
            start(target, TARGET);
            String endpoint = endpoint(target);
            long intent = source.connectPeer(endpoint, TARGET);
            await(() -> manualState(source) == MeshPeerState.ADMITTED);

            // Transport loss alone is transient (mesh-node §7.1): the route
            // ends the admission, and the configured intent stays for Core's
            // reconnect (transport-liveness §6).
            target.close();
            await(() -> !source.isPeerTransportConnected(TARGET));
            await(() -> manualState(source) == MeshPeerState.CONNECTING);
            assertTrue(source.connectionIntentIds().contains(intent));
            assertFalse(source.hasLivePeerIntent(endpoint));

            // Replacement first requests the close; the pump retires the
            // endpoint and, with no selected route left, completes it.
            assertThrows(
                    IllegalStateException.class,
                    () -> source.replacePeerConnection(endpoint, TARGET, 0, null));
            await(() -> !source.isPeerConnectionClosing(intent));
            long replacement = source.replacePeerConnection(endpoint, TARGET, 0, null);
            assertEquals(List.of(replacement), source.connectionIntentIds());
        }
    }

    private static void start(ZLinkJavaRawMeshNode node, RoutingId rid) {
        node.setRoutingId(rid);
        node.setBind("tcp://127.0.0.1:0");
        node.start();
    }

    private static String endpoint(ZLinkJavaRawMeshNode node) throws Exception {
        return ((ZLinkServiceNodeDescriptor) get(node, "localDescriptor")).advertisedEndpoint();
    }

    private static long admittedGeneration(ZLinkJavaRawMeshNode node) {
        try {
            return topology(node)
                    .peer(TARGET)
                    .map(peer -> peer.descriptor().lifecycleGeneration())
                    .orElse(0L);
        } catch (ReflectiveOperationException failure) {
            throw new AssertionError(failure);
        }
    }

    private static MeshPeerState manualState(ZLinkJavaRawMeshNode node) {
        return node.peers().stream()
                .filter(peer -> TARGET.equals(peer.routingId()))
                .map(MeshPeerEntry::state)
                .findFirst()
                .orElse(null);
    }

    private static ZLinkServiceTopologyRegistry topology(ZLinkJavaRawMeshNode node)
            throws ReflectiveOperationException {
        return (ZLinkServiceTopologyRegistry) get(node, "topology");
    }

    private static Object get(Object owner, String name) throws ReflectiveOperationException {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.onSpinWait();
        }
        assertTrue(condition.getAsBoolean(), "condition did not converge");
    }
}
