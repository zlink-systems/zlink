package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.net.ServerSocket;
import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec;

final class ZLinkJavaRawMeshNodeRelocationReplyTest {
    @Test
    void command33PayloadBoundaryMatchesTerminalResult() {
        var success = relay(0, 0);
        assertDoesNotThrow(() ->
            ZLinkJavaRawMeshNode.validateReplyRelayPayload(success, 1));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkJavaRawMeshNode.validateReplyRelayPayload(success, 0));

        var failure = relay(102, 1);
        assertDoesNotThrow(() ->
            ZLinkJavaRawMeshNode.validateReplyRelayPayload(failure, 0));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkJavaRawMeshNode.validateReplyRelayPayload(failure, 1));
    }

    @Test
    void command33And46UseIndependentRawInfrastructureSends()
        throws Exception {
        String endpoint = "tcp://127.0.0.1:" + availableTcpPort();
        String targetEndpoint = "inproc://jvm-relocation-reply-target-"
            + System.nanoTime();
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
            source.setBind(endpoint);
            target.setRoutingId(targetRid);
            target.setBind(targetEndpoint);
            source.setPeerAuthorityResolver(
                (mesh, rid, generation) -> CompletableFuture.completedFuture(
                    java.util.Optional.of(
                        new systems.zlink.framework.runtime.internal.backend
                            .ZLinkInternalMeshNode.PeerAuthorityFence(
                                rid, generation, "peer-owner", 1))));
            target.setPeerAuthorityResolver(
                (mesh, rid, generation) -> CompletableFuture.completedFuture(
                    java.util.Optional.of(
                        new systems.zlink.framework.runtime.internal.backend
                            .ZLinkInternalMeshNode.PeerAuthorityFence(
                                rid, generation, "peer-owner", 1))));
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
            target.start();
            target.connectPeer(endpoint, sourceRid);

            long deadline = System.nanoTime()
                + Duration.ofSeconds(10).toNanos();
            while ((!source.peers().stream().anyMatch(peer ->
                    peer.routingId().equals(targetRid)
                        && peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED)
                    || !target.peers().stream().anyMatch(peer ->
                        peer.routingId().equals(sourceRid)
                            && peer.state()
                                == systems.zlink.framework.runtime.internal.binding.spot
                                    .MeshPeerState.ADMITTED))
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(source.peers().stream().anyMatch(peer ->
                peer.routingId().equals(targetRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED));
            assertTrue(target.peers().stream().anyMatch(peer ->
                peer.routingId().equals(sourceRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED));
            var expectedSource = new ZLinkServiceRelocationWireCodec
                .RequestSourceFence(
                    "source-owner",
                    12,
                    sourceRid,
                    source.lifecycleGeneration());

            byte[] encodedAck;
            try {
                encodedAck = target.requestRelocationReplyRelay(
                        sourceRid,
                        expectedSource,
                        codec.encodeReplyRelay(relay),
                        List.of(payload),
                        Duration.ofSeconds(10))
                    .toCompletableFuture().get(10, TimeUnit.SECONDS);
            } catch (java.util.concurrent.ExecutionException failure) {
                assertTrue(handlerCalled.get(),
                    "command 33 did not reach the source handler");
                throw failure;
            }
            var ack = codec.decodeReplyRelayAck(encodedAck);
            assertEquals(sourceRid, ack.requestSource().nodeRid());
            assertEquals(
                source.lifecycleGeneration(),
                ack.requestSource().nodeGeneration());

            source.setRelocationReplyRelayHandler(
                (transportSource, command, frames) ->
                    new CompletableFuture<>());
            CompletionStage<byte[]> waiting =
                target.requestRelocationReplyRelay(
                    sourceRid,
                    expectedSource,
                    codec.encodeReplyRelay(relay),
                    List.of(payload),
                    Duration.ofMillis(100));
            var collision = assertThrows(
                java.util.concurrent.ExecutionException.class,
                () -> target.requestRelocationReplyRelay(
                        sourceRid,
                        expectedSource,
                        codec.encodeReplyRelay(relay),
                        List.of(payload),
                        Duration.ofMillis(100))
                    .toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertTrue(collision.getCause() instanceof IllegalStateException);
            var timeout = assertThrows(
                java.util.concurrent.ExecutionException.class,
                () -> waiting.toCompletableFuture()
                    .get(1, TimeUnit.SECONDS));
            assertTrue(timeout.getCause()
                instanceof java.util.concurrent.TimeoutException);

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
            var wrongRoute = assertThrows(
                java.util.concurrent.ExecutionException.class,
                () -> target.requestRelocationReplyRelay(
                        sourceRid,
                        expectedSource,
                        codec.encodeReplyRelay(relay),
                        List.of(payload),
                        Duration.ofMillis(100))
                    .toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertTrue(wrongRoute.getCause()
                instanceof java.util.concurrent.TimeoutException);

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
            var stale = assertThrows(
                java.util.concurrent.ExecutionException.class,
                () -> target.requestRelocationReplyRelay(
                        sourceRid,
                        expectedSource,
                        codec.encodeReplyRelay(relay),
                        List.of(payload),
                        Duration.ofMillis(100))
                    .toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertTrue(stale.getCause()
                instanceof java.util.concurrent.TimeoutException);
        }
    }

    private static int availableTcpPort() throws Exception {
        try (var socket = new ServerSocket(0)) {
            socket.setReuseAddress(true);
            return socket.getLocalPort();
        }
    }

    private static ZLinkServiceRelocationWireCodec.ReplyRelay relay(
        int terminalResult,
        int failureCode) {
        return new ZLinkServiceRelocationWireCodec.ReplyRelay(
            new ZLinkServiceRelocationWireCodec.Operation(1, 2),
            3,
            new ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
            6,
            new ZLinkServiceRelocationWireCodec.CoordinatorFence(
                "target-owner",
                7,
                RoutingId.from("target-node"),
                8,
                "version-9"),
            10,
            11,
            terminalResult,
            failureCode);
    }
}
