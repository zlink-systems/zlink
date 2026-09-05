package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledIfEnvironmentVariable;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.sockets.RouterSocket;
import systems.zlink.framework.runtime.internal.binding.spot.MeshNodeState;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkJavaRawMeshNodeShutdownSealTest {
    @Test
    void sealedPeerLossStopsHelloKeepsDrainingAndCompletesDrain() throws Exception {
        var drains = new ZLinkMeshDrainCoordinator(List.of("mesh"));
        try (var context = Zlink.createContext();
             var local = new ZLinkJavaRawMeshNode(context, "mesh");
             var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(local, "seal-local");
            start(peer, "seal-peer");
            local.setPeerAdmissionSealGate(() -> drains.isSealed("mesh"));
            local.connectPeer(endpoint(peer), peer.routingId());
            await(() -> admitted(local) && admitted(peer));
            var claim = drains.tryClaim("mesh");
            assertNotNull(claim);
            drains.sealAll();
            local.markServiceDraining();
            await(() -> remoteState(peer, local.routingId())
                == ZLinkServiceNodeDescriptor.State.DRAINING);
            long revision = descriptor(local).descriptorRevision();
            local.markServiceDraining();
            assertEquals(revision, descriptor(local).descriptorRevision());
            assertFalse(drains.awaitAllZero().toCompletableFuture().isDone());

            String remoteEndpoint = endpoint(peer);
            RoutingId remoteRid = peer.routingId();
            peer.close();
            await(() -> !admitted(local));
            try (var replacement = new ZLinkJavaRawMeshNode(context, "mesh")) {
                replacement.setRoutingId(remoteRid);
                replacement.setBind(remoteEndpoint);
                replacement.start();
                // Install the replacement intent through the existing public
                // owner after it has observed the admitted connection close.
                local.replacePeerConnection(remoteEndpoint, remoteRid,
                    replacement.lifecycleGeneration(), "default");
                var announce = ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                    "announceExpectedPeers", long.class);
                announce.setAccessible(true);
                var next = (java.util.Map<?, ?>) field(local, "nextAnnouncementNanos");
                Object before = next.get(remoteRid);
                for (int i = 0; i < 5; i++) {
                    announce.invoke(local, System.nanoTime() + Duration.ofSeconds(1).toNanos());
                    Thread.sleep(100);
                    assertEquals(MeshNodeState.DRAINING, local.status().state());
                    assertEquals(ZLinkServiceNodeDescriptor.State.DRAINING,
                        descriptor(local).state());
                    assertFalse(admitted(replacement));
                }
                assertEquals(before, next.get(remoteRid), "sealed Hello must not be submitted");
                assertTrue(replacement.peers().isEmpty());
            }
            claim.close();
            drains.awaitAllZero().toCompletableFuture().get(2, TimeUnit.SECONDS);
            assertTimeoutPreemptively(Duration.ofSeconds(5), local::close);
            assertEquals(MeshNodeState.STOPPED, local.status().state());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void unsealedPeerReadmitsIncludingRelocationDraining(boolean draining) throws Exception {
        var drains = new ZLinkMeshDrainCoordinator(List.of("mesh"));
        try (var context = Zlink.createContext();
             var local = new ZLinkJavaRawMeshNode(context, "mesh");
             var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(local, "unsealed-local");
            start(peer, "unsealed-peer");
            local.setPeerAdmissionSealGate(() -> drains.isSealed("mesh"));
            local.connectPeer(endpoint(peer), peer.routingId());
            await(() -> admitted(local) && admitted(peer));
            if (draining) {
                local.markServiceDraining();
                await(() -> remoteState(peer, local.routingId())
                    == ZLinkServiceNodeDescriptor.State.DRAINING);
            }
            String remoteEndpoint = endpoint(peer);
            RoutingId remoteRid = peer.routingId();
            peer.close();
            await(() -> !admitted(local));
            try (var replacement = new ZLinkJavaRawMeshNode(context, "mesh")) {
                replacement.setRoutingId(remoteRid);
                replacement.setBind(remoteEndpoint);
                replacement.start();
                local.replacePeerConnection(remoteEndpoint, remoteRid,
                    replacement.lifecycleGeneration(), "default");
                await(() -> admitted(local) && admitted(replacement));
                assertFalse(drains.isSealed("mesh"));
                assertEquals(draining ? MeshNodeState.DRAINING : MeshNodeState.READY,
                    local.status().state());
                assertEquals(draining ? ZLinkServiceNodeDescriptor.State.DRAINING
                        : ZLinkServiceNodeDescriptor.State.SERVING,
                    remoteState(replacement, local.routingId()));
            }
        }
    }

    @Test
    @EnabledIfEnvironmentVariable(named = "ZLINK_JAVA_STREAM_TRACE", matches = "1")
    void crossedHelloAdmitUsesCompletionDiagnosticOnBothSidesWithoutResettingLiveness()
        throws Exception {
        Logger logger = Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
        var records = new CopyOnWriteArrayList<String>();
        Handler capture = new Handler() {
            @Override public void publish(LogRecord record) { records.add(record.getMessage()); }
            @Override public void flush() { }
            @Override public void close() { }
        };
        logger.addHandler(capture);
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(left, "cross-left");
            start(right, "cross-right");
            left.connectPeer(endpoint(right), right.routingId());
            right.connectPeer(endpoint(left), left.routingId());
            await(() -> admitted(left) && admitted(right));
            String rightEndpoint = endpoint(right);
            await(() -> monitorRegistered(left, "|" + rightEndpoint)
                && monitorRegistered(right, rightEndpoint + "|"));
            // Wire admission can precede the monitor READY edge. Consume
            // its registered connection identity before taking the epoch
            // baseline; the test below concerns identical logical admission,
            // not the monitor's separate pair-validation transition.
            records.clear();
            sendAdmission(left, right, ServiceWireConstants.COMMAND_HELLO);
            sendAdmission(right, left, ServiceWireConstants.COMMAND_HELLO);
            await(() -> completed(records, left, "Hello") && completed(records, right, "Hello"));
            sendAdmission(left, right, ServiceWireConstants.COMMAND_ADMIT);
            sendAdmission(right, left, ServiceWireConstants.COMMAND_ADMIT);
            await(() -> completed(records, left, "Admit") && completed(records, right, "Admit")
                && ready(left, right.routingId()) && ready(right, left.routingId()));
            Object leftEpoch = livenessEpoch(left, right.routingId());
            Object rightEpoch = livenessEpoch(right, left.routingId());
            records.clear();
            // Force the crossed ordering: both Hello completions precede
            // the repeated Admit. Same descriptors and selected Core routes.
            sendAdmission(left, right, ServiceWireConstants.COMMAND_HELLO);
            sendAdmission(right, left, ServiceWireConstants.COMMAND_HELLO);
            await(() -> completed(records, left, "Hello") && completed(records, right, "Hello"));
            sendAdmission(left, right, ServiceWireConstants.COMMAND_ADMIT);
            sendAdmission(right, left, ServiceWireConstants.COMMAND_ADMIT);
            await(() -> completed(records, left, "Admit") && completed(records, right, "Admit"));
            assertSame(leftEpoch, livenessEpoch(left, right.routingId()));
            assertSame(rightEpoch, livenessEpoch(right, left.routingId()));
            assertEquals(1, left.peers().size());
            assertEquals(1, right.peers().size());
            assertFalse(records.stream().anyMatch(s -> s.startsWith("ZLINK_FRAMEWORK_PEER_READY")));
            assertFalse(records.stream().anyMatch(s -> s.contains("admission-invalid")
                || s.contains("admission-rejected") || s.contains("duplicate-admission-reject")));
        } finally {
            logger.removeHandler(capture);
        }
    }

    private static boolean completed(List<String> records, ZLinkJavaRawMeshNode node, String command) {
        return records.stream().anyMatch(s -> s.contains("rid=" + node.routingId() + " ")
            && s.contains("mesh_peer_admission_accepted") && s.contains("command=" + command));
    }

    private static void sendAdmission(ZLinkJavaRawMeshNode from, ZLinkJavaRawMeshNode to, int command)
        throws Exception {
        var port = (ZLinkJavaRawServicePort) field(from, "port");
        port.send((RouterSocket) field(from, "router"), to.routingId(),
            List.of(new ZLinkServiceM6AWireCodec().encodeAdmission(command, descriptor(from))))
            .toCompletableFuture().get(2, TimeUnit.SECONDS);
    }

    private static boolean monitorRegistered(ZLinkJavaRawMeshNode node, String endpointPair) {
        try {
            return ((java.util.Map<?, ?>) field(node, "monitorConnectionIds")).containsKey(endpointPair);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    private static boolean ready(ZLinkJavaRawMeshNode node, RoutingId peer) {
        try {
            var method = ZLinkJavaRawMeshNode.class.getDeclaredMethod("isReadyPeer", RoutingId.class);
            method.setAccessible(true);
            return (boolean) method.invoke(node, peer);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    private static Object livenessEpoch(ZLinkJavaRawMeshNode node, RoutingId peer) throws Exception {
        return ((java.util.Map<?, ?>) field(field(node, "liveness"), "peers")).get(peer);
    }

    private static void start(ZLinkJavaRawMeshNode node, String name) {
        node.setRoutingId(RoutingId.from(name));
        node.setBind("inproc://" + name + "-" + System.nanoTime());
        node.start();
    }

    private static String endpoint(ZLinkJavaRawMeshNode node) throws Exception {
        return descriptor(node).advertisedEndpoint();
    }

    private static ZLinkServiceNodeDescriptor descriptor(ZLinkJavaRawMeshNode node) throws Exception {
        return (ZLinkServiceNodeDescriptor) field(node, "localDescriptor");
    }

    private static ZLinkServiceNodeDescriptor.State remoteState(ZLinkJavaRawMeshNode node, RoutingId peer) {
        try {
            return ((ZLinkServiceTopologyRegistry) field(node, "topology")).peer(peer)
                .map(p -> p.descriptor().state()).orElse(null);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    private static boolean admitted(ZLinkJavaRawMeshNode node) {
        return node.peers().stream().anyMatch(p -> p.state() == MeshPeerState.ADMITTED);
    }

    private static Object field(Object owner, String name) throws ReflectiveOperationException {
        Field field = owner.getClass().getDeclaredField(name);
        field.setAccessible(true);
        return field.get(owner);
    }

    private static void await(BooleanSupplier condition) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        while (!condition.getAsBoolean() && System.nanoTime() < deadline) {
            Thread.sleep(5);
        }
        assertTrue(condition.getAsBoolean(), "condition did not converge");
    }
}
