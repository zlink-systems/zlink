package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceNodeDescriptor;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceTopologyRegistry;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.function.Consumer;
import java.util.function.Supplier;

final class ZLinkJavaRawMeshNodeRelocationReplyTest {
    @Test
    void relocationForwardRequiresServiceAdmissionOnTheExactPeerGeneration() throws Exception {
        var localRid = RoutingId.from("relocation-barrier-source");
        var peerRid = RoutingId.from("relocation-barrier-target");
        try (var context = Zlink.createContext();
                var port = new ZLinkJavaRawServicePort(context)) {
            var router = port.openRouter(localRid);
            var local = descriptor(localRid, 1);
            var peer = descriptor(peerRid, 2);
            var topology = new ZLinkServiceTopologyRegistry(local, port.stateLane());
            assertEquals(
                    ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                    topology.admit(peer, "1", ServiceWireConstants.COMMAND_HELLO));
            try (var source =
                            ZLinkJavaRawMeshNodeTesting.create(
                                    "mesh",
                                    port,
                                    router,
                                    local,
                                    topology,
                                    Map.of(peerRid, 1L),
                                    Runnable::run);
                    var part = Message.from("payload")) {
                var target =
                        new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                "target-spot", 1, peerRid, peer.lifecycleGeneration(), 1, 1);
                var stale =
                        new ZLinkServiceM6BWireCodec.SpotMessage(
                                false, 0, null, "source-spot", target);
                var parts = List.of(part);
                assertFalse(
                        source.forwardRelocationSpot(
                                stale, target, null, parts, ignored -> {}, ignored -> {}),
                        "a HELLO descriptor alone must not cross the relocation route barrier");
                assertEquals(
                        ZLinkServiceTopologyRegistry.AdmissionResult.ADMITTED,
                        topology.admit(peer, "1", ServiceWireConstants.COMMAND_ADMIT));
                var wrongGeneration =
                        new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                "target-spot", 1, peerRid, peer.lifecycleGeneration() + 1, 1, 1);
                assertFalse(
                        source.forwardRelocationSpot(
                                stale, wrongGeneration, null, parts, ignored -> {}, ignored -> {}));
                assertTrue(
                        source.forwardRelocationSpot(
                                stale, target, null, parts, ignored -> {}, ignored -> {}),
                        "the matching admitted peer may cross the relocation route barrier");
            }
        }
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId rid, long generation) {
        return new ZLinkServiceNodeDescriptor(
                "mesh",
                rid,
                generation,
                1,
                "tcp://127.0.0.1:12345",
                List.of(),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                1,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                1,
                1,
                100,
                0,
                0);
    }

    @Test
    void command33PayloadBoundaryMatchesTerminalResult() {
        var success = relay(0, 0);
        assertDoesNotThrow(() -> ZLinkJavaRawMeshNode.validateReplyRelayPayload(success, 1));
        assertThrows(
                IllegalArgumentException.class,
                () -> ZLinkJavaRawMeshNode.validateReplyRelayPayload(success, 0));

        var failure = relay(102, 1);
        assertDoesNotThrow(() -> ZLinkJavaRawMeshNode.validateReplyRelayPayload(failure, 0));
        assertThrows(
                IllegalArgumentException.class,
                () -> ZLinkJavaRawMeshNode.validateReplyRelayPayload(failure, 1));
    }

    @Test
    void command33And46UseIndependentRawInfrastructureSends() throws Exception {
        String targetEndpoint = "inproc://jvm-relocation-reply-target-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-reply-source");
        RoutingId targetRid = RoutingId.from("jvm-reply-target");
        var codec = new ZLinkServiceRelocationWireCodec();
        var relay = relay(0, 0);
        byte[] payload = new byte[] {1, 2, 3};
        var handlerCalled = new AtomicBoolean();

        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("tcp://127.0.0.1:0");
            target.setRoutingId(targetRid);
            target.setBind(targetEndpoint);
            source.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "peer-owner", 1))));
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "source-owner", 12))));
            source.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) -> {
                        handlerCalled.set(true);
                        return CompletableFuture.completedFuture(
                                codec.encodeReplyRelayAck(
                                        new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                                relay.relocation(),
                                                relay.coordinator(),
                                                relay.operation(),
                                                relay.replyRouteId(),
                                                new ZLinkServiceRelocationWireCodec
                                                        .RequestSourceFence(
                                                        "source-owner",
                                                        12,
                                                        sourceRid,
                                                        source.lifecycleGeneration()),
                                                1)));
                    });
            source.start();
            String endpoint = source.status().localEndpoint();
            target.start();
            target.connectPeer(endpoint, sourceRid);

            long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
            while ((!source.peers().stream()
                                    .anyMatch(
                                            peer ->
                                                    peer.routingId().equals(targetRid)
                                                            && peer.state()
                                                                    == systems.zlink.framework
                                                                            .runtime.internal
                                                                            .binding.spot
                                                                            .MeshPeerState.ADMITTED)
                            || !target.peers().stream()
                                    .anyMatch(
                                            peer ->
                                                    peer.routingId().equals(sourceRid)
                                                            && peer.state()
                                                                    == systems.zlink.framework
                                                                            .runtime.internal
                                                                            .binding.spot
                                                                            .MeshPeerState
                                                                            .ADMITTED))
                    && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(
                    source.peers().stream()
                            .anyMatch(
                                    peer ->
                                            peer.routingId().equals(targetRid)
                                                    && peer.state()
                                                            == systems.zlink.framework.runtime
                                                                    .internal.binding.spot
                                                                    .MeshPeerState.ADMITTED));
            assertTrue(
                    target.peers().stream()
                            .anyMatch(
                                    peer ->
                                            peer.routingId().equals(sourceRid)
                                                    && peer.state()
                                                            == systems.zlink.framework.runtime
                                                                    .internal.binding.spot
                                                                    .MeshPeerState.ADMITTED));
            var expectedSource =
                    new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                            "source-owner", 12, sourceRid, source.lifecycleGeneration());

            byte[] encodedAck;
            try {
                encodedAck =
                        target.requestRelocationReplyRelay(
                                        sourceRid,
                                        expectedSource,
                                        codec.encodeReplyRelay(relay),
                                        List.of(payload),
                                        Duration.ofSeconds(10))
                                .toCompletableFuture()
                                .get(10, TimeUnit.SECONDS);
            } catch (ExecutionException failure) {
                assertTrue(handlerCalled.get(), "command 33 did not reach the source handler");
                throw failure;
            }
            var ack = codec.decodeReplyRelayAck(encodedAck);
            assertEquals(sourceRid, ack.requestSource().nodeRid());
            assertEquals(source.lifecycleGeneration(), ack.requestSource().nodeGeneration());

            source.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) -> new CompletableFuture<>());
            CompletionStage<byte[]> waiting =
                    target.requestRelocationReplyRelay(
                            sourceRid,
                            expectedSource,
                            codec.encodeReplyRelay(relay),
                            List.of(payload),
                            Duration.ofMillis(100));
            var collision =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    sourceRid,
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(100))
                                            .toCompletableFuture()
                                            .get(1, TimeUnit.SECONDS));
            assertTrue(collision.getCause() instanceof IllegalStateException);
            var timeout =
                    assertThrows(
                            ExecutionException.class,
                            () -> waiting.toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    ((ZLinkFrameworkException) timeout.getCause()).kind());

            source.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) ->
                            CompletableFuture.completedFuture(
                                    codec.encodeReplyRelayAck(
                                            new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                                    relay.relocation(),
                                                    relay.coordinator(),
                                                    relay.operation(),
                                                    relay.replyRouteId() + 1,
                                                    expectedSource,
                                                    1))));
            var wrongRoute =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    sourceRid,
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(100))
                                            .toCompletableFuture()
                                            .get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    ((ZLinkFrameworkException) wrongRoute.getCause()).kind());

            source.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) ->
                            CompletableFuture.completedFuture(
                                    codec.encodeReplyRelayAck(
                                            new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                                    relay.relocation(),
                                                    relay.coordinator(),
                                                    relay.operation(),
                                                    relay.replyRouteId(),
                                                    new ZLinkServiceRelocationWireCodec
                                                            .RequestSourceFence(
                                                            "source-owner",
                                                            12,
                                                            sourceRid,
                                                            source.lifecycleGeneration() + 1),
                                                    1))));
            var stale =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    sourceRid,
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(100))
                                            .toCompletableFuture()
                                            .get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    ((ZLinkFrameworkException) stale.getCause()).kind());
        }
    }

    @Test
    void command33LandsOnRelocationSourceDistinctFromRequestSourceFence() throws Exception {
        String targetEndpoint = "inproc://jvm-relocation-reply-landing-" + System.nanoTime();
        RoutingId landingRid = RoutingId.from("jvm-reply-landing");
        RoutingId targetRid = RoutingId.from("jvm-reply-relayer");
        RoutingId requesterRid = RoutingId.from("jvm-reply-requester");
        var codec = new ZLinkServiceRelocationWireCodec();
        var relay = relay(0, 0);
        byte[] payload = new byte[] {1, 2, 3};

        try (var context = Zlink.createContext();
                var landing = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            landing.setRoutingId(landingRid);
            landing.setBind("tcp://127.0.0.1:0");
            target.setRoutingId(targetRid);
            target.setBind(targetEndpoint);
            landing.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "peer-owner", 1))));
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "peer-owner", 1))));
            //  The request-source fence names a third node that is not the
            //  transport destination of command 33.
            var expectedSource =
                    new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                            "request-owner", 12, requesterRid, 41);
            landing.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) ->
                            CompletableFuture.completedFuture(
                                    codec.encodeReplyRelayAck(
                                            new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                                    relay.relocation(),
                                                    relay.coordinator(),
                                                    relay.operation(),
                                                    relay.replyRouteId(),
                                                    expectedSource,
                                                    1))));
            landing.start();
            String endpoint = landing.status().localEndpoint();
            target.start();
            target.connectPeer(endpoint, landingRid);

            long deadline = System.nanoTime() + Duration.ofSeconds(10).toNanos();
            while ((!landing.peers().stream()
                                    .anyMatch(
                                            peer ->
                                                    peer.routingId().equals(targetRid)
                                                            && peer.state()
                                                                    == systems.zlink.framework
                                                                            .runtime.internal
                                                                            .binding.spot
                                                                            .MeshPeerState.ADMITTED)
                            || !target.peers().stream()
                                    .anyMatch(
                                            peer ->
                                                    peer.routingId().equals(landingRid)
                                                            && peer.state()
                                                                    == systems.zlink.framework
                                                                            .runtime.internal
                                                                            .binding.spot
                                                                            .MeshPeerState
                                                                            .ADMITTED))
                    && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }

            var intermediate =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    landingRid,
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(100))
                                            .toCompletableFuture()
                                            .get(1, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    ((ZLinkFrameworkException) intermediate.getCause()).kind());

            //  An ACK that claims a different request-source fence (here the
            //  landing itself) can never close the pending command 33.
            landing.setRelocationReplyRelayHandler(
                    (transportSource, command, frames) ->
                            CompletableFuture.completedFuture(
                                    codec.encodeReplyRelayAck(
                                            new ZLinkServiceRelocationWireCodec.ReplyRelayAck(
                                                    relay.relocation(),
                                                    relay.coordinator(),
                                                    relay.operation(),
                                                    relay.replyRouteId(),
                                                    new ZLinkServiceRelocationWireCodec
                                                            .RequestSourceFence(
                                                            "request-owner", 12, landingRid, 41),
                                                    1))));
            var wrongFence =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    landingRid,
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(200))
                                            .toCompletableFuture()
                                            .get(2, TimeUnit.SECONDS));
            assertEquals(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    ((ZLinkFrameworkException) wrongFence.getCause()).kind());

            //  A landing that is not an admitted peer never receives the
            //  relay; the pending fence fails or expires without an ACK.
            var wrongLanding =
                    assertThrows(
                            ExecutionException.class,
                            () ->
                                    target.requestRelocationReplyRelay(
                                                    RoutingId.from("jvm-reply-absent"),
                                                    expectedSource,
                                                    codec.encodeReplyRelay(relay),
                                                    List.of(payload),
                                                    Duration.ofMillis(200))
                                            .toCompletableFuture()
                                            .get(2, TimeUnit.SECONDS));
            assertNotNull(wrongLanding.getCause());
        }
    }

    @Test
    void originalRequesterAcceptsAndAuthenticatesTheStoredTerminal() throws Exception {
        var requesterRid = RoutingId.from("reply-requester");
        var landingRid = RoutingId.from("reply-acceptor");
        var writerRid = RoutingId.from("reply-writer");
        var seen = new CompletableFuture<ZLinkServiceM6BWireCodec.ActorMessage>();
        try (var context = Zlink.createContext();
                var requester = new ZLinkJavaRawMeshNode(context, "mesh");
                var landing = new ZLinkJavaRawMeshNode(context, "mesh");
                var writer = new ZLinkJavaRawMeshNode(context, "mesh")) {
            startReplyPeer(requester, requesterRid);
            requester.setPeerAuthorityResolver(
                    new ZLinkInternalMeshNode.PeerAuthorityResolver() {
                        @Override
                        public CompletionStage<Optional<ZLinkInternalMeshNode.PeerAuthorityFence>>
                                resolve(String mesh, RoutingId peer, long generation) {
                            return CompletableFuture.completedFuture(
                                    Optional.of(
                                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    peer, generation, "owner", 1)));
                        }

                        @Override
                        public CompletionStage<Boolean> confirmsRelocationReply(
                                ZLinkInternalMeshNode.PeerAuthorityFence source,
                                ZLinkServiceRelocationWireCodec.ReplyRelay relay,
                                List<byte[]> payload) {
                            return CompletableFuture.completedFuture(payload.getFirst()[0] == 11);
                        }
                    });
            startReplyPeer(landing, landingRid);
            startReplyPeer(writer, writerRid);
            requester.connectPeer(landing.status().localEndpoint(), landingRid);
            writer.connectPeer(landing.status().localEndpoint(), landingRid);
            requester.connectPeer(writer.status().localEndpoint(), writerRid);
            awaitReplyPeers(requester, landingRid, writerRid);
            awaitReplyPeers(landing, requesterRid, writerRid);
            awaitReplyPeers(writer, requesterRid, landingRid);

            landing.spotNode()
                    .setRelocationStagingIngressHandler(
                            new ZLinkInternalSpotNode.RelocationStagingIngressHandler() {
                                @Override
                                public boolean handleSpot(
                                        ZLinkInternalMeshNode.PeerAuthorityFence source,
                                        ZLinkServiceM6BWireCodec.SpotMessage header,
                                        byte[] metadata,
                                        Supplier<byte[]> frozen,
                                        int size,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure) {
                                    return false;
                                }

                                @Override
                                public boolean handleActor(
                                        ZLinkInternalMeshNode.PeerAuthorityFence source,
                                        ZLinkServiceM6BWireCodec.ActorMessage header,
                                        Supplier<byte[]> frozen,
                                        List<Message> parts,
                                        String contentType,
                                        Consumer<List<Message>> reply,
                                        Consumer<Throwable> failure) {
                                    Message.closeAll(parts);
                                    seen.complete(header);
                                    return true;
                                }
                            });
            var actor = new ZLinkBackendActorRef(landingRid, "relocating-actor", 1);
            requester.spotNode().rememberActorAuthority(actor, 1, 1, landing.lifecycleGeneration());
            try (var part = Message.from("request")) {
                var operation =
                        requester.requestActor(
                                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                        actor, landing.lifecycleGeneration(), 1, 1),
                                List.of(part),
                                Duration.ofSeconds(5),
                                null);
                var original = seen.get(5, TimeUnit.SECONDS);
                var reply =
                        new ZLinkServiceRelocationWireCodec.ReplyRelay(
                                new ZLinkServiceRelocationWireCodec.Operation(
                                        original.operationHigh(), original.operationLow()),
                                original.correlation(),
                                new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                                6,
                                new ZLinkServiceRelocationWireCodec.CoordinatorFence(
                                        "owner",
                                        1,
                                        writerRid,
                                        writer.lifecycleGeneration(),
                                        "version"),
                                1,
                                1,
                                0,
                                0);
                var codec = new ZLinkServiceRelocationWireCodec();
                var source =
                        new ZLinkServiceRelocationWireCodec.RequestSourceFence(
                                "owner", 1, requesterRid, requester.lifecycleGeneration());
                var payload = new byte[] {11, 12, 13};
                for (int expected : List.of(1, 2)) {
                    var ack =
                            codec.decodeReplyRelayAck(
                                    writer.requestRelocationReplyRelay(
                                                    requesterRid,
                                                    source,
                                                    codec.encodeReplyRelay(reply),
                                                    List.of(payload),
                                                    Duration.ofSeconds(2))
                                            .toCompletableFuture()
                                            .get(3, TimeUnit.SECONDS));
                    assertEquals(expected, ack.status());
                    assertEquals(source, ack.requestSource());
                }
                var value = operation.toCompletableFuture().get(5, TimeUnit.SECONDS);
                try {
                    assertArrayEquals(payload, value.getFirst().toByteArray());
                } finally {
                    Message.closeAll(value);
                }
            }
        }
    }

    private static void startReplyPeer(ZLinkJavaRawMeshNode node, RoutingId rid) {
        node.setRoutingId(rid);
        node.setBind("tcp://127.0.0.1:0");
        node.setPeerAuthorityResolver(
                (mesh, peer, generation) ->
                        CompletableFuture.completedFuture(
                                Optional.of(
                                        new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                peer, generation, "owner", 1))));
        node.start();
        node.confirmLocalOwnerFence(
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                        rid, node.lifecycleGeneration(), "owner", 1));
    }

    private static void awaitReplyPeers(ZLinkJavaRawMeshNode node, RoutingId... expected)
            throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        for (var rid : expected) {
            while (!node.peers().stream()
                    .anyMatch(
                            peer ->
                                    peer.routingId().equals(rid)
                                            && peer.state().name().equals("ADMITTED"))) {
                assertTrue(System.nanoTime() < deadline, "reply peer was not admitted");
                Thread.sleep(1);
            }
        }
    }

    private static ZLinkServiceRelocationWireCodec.ReplyRelay relay(
            int terminalResult, int failureCode) {
        return new ZLinkServiceRelocationWireCodec.ReplyRelay(
                new ZLinkServiceRelocationWireCodec.Operation(1, 2),
                3,
                new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                6,
                new ZLinkServiceRelocationWireCodec.CoordinatorFence(
                        "target-owner", 7, RoutingId.from("target-node"), 8, "version-9"),
                10,
                11,
                terminalResult,
                failureCode);
    }
}
