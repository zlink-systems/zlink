package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
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

import java.lang.reflect.Field;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

final class ZLinkJavaRawMeshNodeShutdownSealTest {
    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void inboundHelloFollowsHostShutdownSeal(boolean sealed) throws Exception {
        var drains = new ZLinkMeshDrainCoordinator(List.of("mesh"));
        var helloObserved = new CountDownLatch(1);
        try (var context = Zlink.createContext();
                var local = new ZLinkJavaRawMeshNode(context, "mesh");
                var remote = new ZLinkJavaRawServicePort(context)) {
            start(local, "inbound-seal-local");
            local.setPeerAdmissionSealGate(
                    () -> {
                        boolean result = drains.isSealed("mesh");
                        helloObserved.countDown();
                        return result;
                    });
            if (sealed) {
                drains.sealAll();
                local.markServiceDraining();
            }
            RoutingId peerRid = RoutingId.from("inbound-seal-peer");
            RouterSocket peer = remote.openRouter(peerRid);
            peer.options().probe(true);
            peer.connect(endpoint(local));
            var descriptor =
                    new ZLinkServiceNodeDescriptor(
                            "mesh",
                            peerRid,
                            7,
                            1,
                            "inproc://inbound-seal-peer",
                            List.of(),
                            ZLinkServiceNodeDescriptor.State.SERVING,
                            "default",
                            1,
                            List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                            ZLinkServiceNodeDescriptor.ObjectRole.NONE,
                            1,
                            1,
                            0,
                            0,
                            0);
            var wire = new ZLinkServiceM6AWireCodec();
            remote.send(
                            peer,
                            local.routingId(),
                            List.of(
                                    wire.encodeAdmission(
                                            ServiceWireConstants.COMMAND_HELLO, descriptor)))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            assertTrue(helloObserved.await(2, TimeUnit.SECONDS));
            if (sealed) {
                long deadline = System.nanoTime() + Duration.ofMillis(200).toNanos();
                do {
                    var inbound = remote.receive(peer);
                    if (inbound.isPresent()) {
                        try (var message = inbound.orElseThrow()) {
                            // ROUTER probe is an empty transport frame, not admission.
                            assertEquals(
                                    -1,
                                    ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                                            message.frames()),
                                    "sealed Hello must be unanswered");
                        }
                    }
                    Thread.sleep(1);
                } while (System.nanoTime() < deadline);
                assertTrue(local.peers().isEmpty());
                assertTrue(
                        ((ZLinkServiceTopologyRegistry) field(local, "topology"))
                                .peer(peerRid)
                                .isEmpty());
                assertTrue(((java.util.Map<?, ?>) field(local, "admittedPeerChannels")).isEmpty());
                assertTrue(((java.util.Map<?, ?>) field(local, "peerIntentRoutingIds")).isEmpty());
                assertTrue(((java.util.Set<?>) field(local, "rejectedPeers")).isEmpty());
            } else {
                var receivedAdmit = new java.util.concurrent.atomic.AtomicBoolean();
                await(
                        () -> {
                            if (receivedAdmit.get()) {
                                return true;
                            }
                            var inbound = remote.receive(peer);
                            if (inbound.isEmpty()) {
                                return false;
                            }
                            try (var message = inbound.orElseThrow()) {
                                receivedAdmit.set(
                                        ZLinkJavaRawMeshNode.allowedInfrastructureControlCommand(
                                                        message.frames())
                                                == ServiceWireConstants.COMMAND_ADMIT);
                                return receivedAdmit.get();
                            }
                        });
                assertEquals(ZLinkServiceNodeDescriptor.State.SERVING, remoteState(local, peerRid));
                assertEquals(1, local.peers().size());
            }
        }
    }

    @Test
    void admittedPeersStillExchangeDrainingUpdatesAfterSeal() throws Exception {
        var drains = new ZLinkMeshDrainCoordinator(List.of("mesh"));
        try (var context = Zlink.createContext();
                var local = new ZLinkJavaRawMeshNode(context, "mesh");
                var peer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(local, "sealed-update-local");
            start(peer, "sealed-update-peer");
            local.setPeerAdmissionSealGate(() -> drains.isSealed("mesh"));
            local.connectPeer(endpoint(peer), peer.routingId());
            await(() -> admitted(local) && admitted(peer));
            drains.sealAll();
            local.markServiceDraining();
            await(
                    () ->
                            remoteState(peer, local.routingId())
                                    == ZLinkServiceNodeDescriptor.State.DRAINING);
            var current = descriptor(peer);
            var laterHello =
                    new ZLinkServiceNodeDescriptor(
                            current.meshName(),
                            current.nodeRoutingId(),
                            current.lifecycleGeneration(),
                            99,
                            current.advertisedEndpoint(),
                            current.channels(),
                            current.state(),
                            current.securityIdentity(),
                            current.applicationVersion(),
                            current.protocolCapabilities(),
                            current.objectRole(),
                            current.placementWeight(),
                            current.activeCapacityLimit(),
                            current.pendingCapacityLimit(),
                            current.activeCapacityUsed(),
                            current.pendingCapacityUsed());
            var port = (ZLinkJavaRawServicePort) field(peer, "port");
            port.send(
                            (RouterSocket) field(peer, "router"),
                            local.routingId(),
                            List.of(
                                    new ZLinkServiceM6AWireCodec()
                                            .encodeAdmission(
                                                    ServiceWireConstants.COMMAND_HELLO,
                                                    laterHello)))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            // If the sealed Hello advances revision to 99, the next Update
            // cannot install its lower revision and this wait fails.
            peer.markServiceDraining();
            await(
                    () ->
                            remoteState(local, peer.routingId())
                                    == ZLinkServiceNodeDescriptor.State.DRAINING);
            assertEquals(
                    descriptor(peer).descriptorRevision(),
                    ((ZLinkServiceTopologyRegistry) field(local, "topology"))
                            .peer(peer.routingId())
                            .orElseThrow()
                            .descriptor()
                            .descriptorRevision());
            assertEquals(MeshNodeState.DRAINING, local.status().state());
            assertEquals(1, local.peers().size());
            assertTrue(drains.isSealed("mesh"));
        }
    }

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
            await(
                    () ->
                            remoteState(peer, local.routingId())
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
                // Transport loss leaves the intent open for Core's reconnect
                // (mesh-node §7.1); replacement closes it through the owner.
                replaceAfterClose(
                        local, remoteEndpoint, remoteRid, replacement.lifecycleGeneration());
                var announce =
                        ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                                "announceExpectedPeers", long.class);
                announce.setAccessible(true);
                var next = (java.util.Map<?, ?>) field(local, "nextAnnouncementNanos");
                Object before = next.get(remoteRid);
                for (int i = 0; i < 5; i++) {
                    announce.invoke(local, System.nanoTime() + Duration.ofSeconds(1).toNanos());
                    Thread.sleep(100);
                    assertEquals(MeshNodeState.DRAINING, local.status().state());
                    assertEquals(
                            ZLinkServiceNodeDescriptor.State.DRAINING, descriptor(local).state());
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
                await(
                        () ->
                                remoteState(peer, local.routingId())
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
                replaceAfterClose(
                        local, remoteEndpoint, remoteRid, replacement.lifecycleGeneration());
                await(() -> admitted(local) && admitted(replacement));
                assertFalse(drains.isSealed("mesh"));
                assertEquals(
                        draining ? MeshNodeState.DRAINING : MeshNodeState.READY,
                        local.status().state());
                assertEquals(
                        draining
                                ? ZLinkServiceNodeDescriptor.State.DRAINING
                                : ZLinkServiceNodeDescriptor.State.SERVING,
                        remoteState(replacement, local.routingId()));
            }
        }
    }

    @Test
    void repeatedHelloOrAdmitOnTheSelectedConnectionNeverClearsAdmissionReadiness()
            throws Exception {
        try (var context = Zlink.createContext();
                var left = new ZLinkJavaRawMeshNode(context, "mesh");
                var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            start(left, "repeat-left");
            start(right, "repeat-right");
            left.connectPeer(endpoint(right), right.routingId());
            await(() -> admitted(left) && admitted(right));
            await(() -> ready(right, left.routingId()));
            // Observe the admission-ready marker itself: the transient a
            // concurrent peers()/send caller could hit lies inside one
            // dispatch turn, so sampling readiness from outside cannot
            // pin it deterministically.
            var field =
                    ZLinkJavaRawMeshNode.class.getDeclaredField("admissionControlReadyConnections");
            field.setAccessible(true);
            @SuppressWarnings("unchecked")
            var readiness =
                    new RecordingReadiness((java.util.Map<RoutingId, String>) field.get(right));
            field.set(right, readiness);
            for (int command :
                    new int[] {
                        ServiceWireConstants.COMMAND_HELLO,
                        ServiceWireConstants.COMMAND_ADMIT,
                        ServiceWireConstants.COMMAND_HELLO
                    }) {
                long marked = readiness.marks(left.routingId());
                // Same descriptor, same selected connection: an idempotent
                // admission (mesh-node §7.1). Its completion re-marks the
                // connection (Admit received, or the Admit reply sent).
                sendAdmission(left, right, command);
                await(() -> readiness.marks(left.routingId()) > marked);
                assertEquals(
                        0,
                        readiness.clears(left.routingId()),
                        "command " + command + " cleared admission readiness");
                assertTrue(ready(right, left.routingId()));
            }
            assertEquals(1, right.peers().size());
        }
    }

    private static final class RecordingReadiness
            extends java.util.concurrent.ConcurrentHashMap<RoutingId, String> {
        private final java.util.Map<RoutingId, Integer> marks =
                new java.util.concurrent.ConcurrentHashMap<>();
        private final java.util.Map<RoutingId, Integer> clears =
                new java.util.concurrent.ConcurrentHashMap<>();

        RecordingReadiness(java.util.Map<RoutingId, String> current) {
            super(current);
        }

        @Override
        public String put(RoutingId key, String value) {
            marks.merge(key, 1, Integer::sum);
            return super.put(key, value);
        }

        @Override
        public String remove(Object key) {
            clears.merge((RoutingId) key, 1, Integer::sum);
            return super.remove(key);
        }

        @Override
        public boolean remove(Object key, Object value) {
            clears.merge((RoutingId) key, 1, Integer::sum);
            return super.remove(key, value);
        }

        int marks(RoutingId peer) {
            return marks.getOrDefault(peer, 0);
        }

        int clears(RoutingId peer) {
            return clears.getOrDefault(peer, 0);
        }
    }

    private static boolean completed(
            List<String> records, ZLinkJavaRawMeshNode node, String command) {
        return records.stream()
                .anyMatch(
                        s ->
                                s.contains("rid=" + node.routingId() + " ")
                                        && s.contains("mesh_peer_admission_accepted")
                                        && s.contains("command=" + command));
    }

    private static void sendAdmission(
            ZLinkJavaRawMeshNode from, ZLinkJavaRawMeshNode to, int command) throws Exception {
        var port = (ZLinkJavaRawServicePort) field(from, "port");
        port.send(
                        (RouterSocket) field(from, "router"),
                        to.routingId(),
                        List.of(
                                new ZLinkServiceM6AWireCodec()
                                        .encodeAdmission(command, descriptor(from))))
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
    }

    private static boolean ready(ZLinkJavaRawMeshNode node, RoutingId peer) {
        try {
            var method =
                    ZLinkJavaRawMeshNode.class.getDeclaredMethod("isReadyPeer", RoutingId.class);
            method.setAccessible(true);
            return (boolean) method.invoke(node, peer);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    private static Object livenessEpoch(ZLinkJavaRawMeshNode node, RoutingId peer)
            throws Exception {
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

    private static ZLinkServiceNodeDescriptor descriptor(ZLinkJavaRawMeshNode node)
            throws Exception {
        return (ZLinkServiceNodeDescriptor) field(node, "localDescriptor");
    }

    private static ZLinkServiceNodeDescriptor.State remoteState(
            ZLinkJavaRawMeshNode node, RoutingId peer) {
        try {
            return ((ZLinkServiceTopologyRegistry) field(node, "topology"))
                    .peer(peer)
                    .map(p -> p.descriptor().state())
                    .orElse(null);
        } catch (ReflectiveOperationException e) {
            throw new AssertionError(e);
        }
    }

    private static boolean admitted(ZLinkJavaRawMeshNode node) {
        return node.peers().stream().anyMatch(p -> p.state() == MeshPeerState.ADMITTED);
    }

    // A transport loss leaves the intent open for Core's reconnect (mesh-node
    // §7.1). The first call requests closure; Core's selected-route result
    // signals when that closure has completed.
    private static long replaceAfterClose(
            ZLinkJavaRawMeshNode node, String endpoint, RoutingId rid, long lifecycleGeneration)
            throws Exception {
        long oldIntent = node.connectionIntentIds().get(0);
        assertThrows(
                IllegalStateException.class,
                () -> node.replacePeerConnection(endpoint, rid, lifecycleGeneration, "default"));
        await(() -> !node.isPeerConnectionClosing(oldIntent));
        return node.replacePeerConnection(endpoint, rid, lifecycleGeneration, "default");
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
