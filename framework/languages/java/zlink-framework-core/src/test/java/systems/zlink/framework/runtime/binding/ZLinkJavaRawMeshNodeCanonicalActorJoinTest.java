package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

final class ZLinkJavaRawMeshNodeCanonicalActorJoinTest {
    @Test
    void unboundJavaToJavaActorJoinUsesCanonical28AndReturnsTargetAdmissionReply()
        throws Exception {
        String endpoint = "inproc://jvm-canonical-actor-join-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-canonical-source");
        RoutingId targetRid = RoutingId.from("jvm-canonical-target");
        AtomicReference<ServiceWirePilotCodec.ActorJoin28> received =
            new AtomicReference<>();
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-canonical-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver((mesh, rid, generation) ->
                CompletableFuture.completedFuture(generation == 0
                    ? Optional.empty()
                    : Optional.of(new ZLinkInternalMeshNode.PeerAuthorityFence(
                        rid, generation, "source-owner", 1L))));
            target.setCanonicalActorJoinHandler((sourcePeer, join) -> {
                assertEquals(sourceRid, sourcePeer);
                received.set(join);
                return CompletableFuture.completedFuture(
                    new ZLinkInternalMeshNode.CanonicalActorJoinResponse(
                        true, 7L, List.of(Message.from("target-reply"))));
            });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            ((ZLinkJavaRawSpotNode) source.spotNode()).rememberSpotAuthority(
                targetRid, "target-room", 5L, 9L, 11L);
            var request = new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                new ZLinkBackendActorRef(sourceRid, "actor-a", 3L),
                source.lifecycleGeneration(), 1L, 2L,
                targetRid, target.lifecycleGeneration(), "target-room", 5L,
                9L, 11L, false,
                "ZLinkFrameworkActorJoinRequest", "application/json",
                "{\"join\":true}".getBytes(StandardCharsets.UTF_8));

            assertTrue(source.canRequestCanonicalActorJoin(request));
            var reply = source.requestCanonicalActorJoin(
                    request, Duration.ofSeconds(1))
                .toCompletableFuture().get(1, TimeUnit.SECONDS);

            assertTrue(reply.accepted());
            assertEquals(32_768L, reply.receiveChunkLimitBytes(),
                "receiver owns the canonical admission reply chunk limit");
            assertEquals("target-reply", reply.applicationReply().getFirst().toUtf8String());
            reply.applicationReply().forEach(Message::close);
            ServiceWirePilotCodec.ActorJoin28 join = received.get();
            assertEquals("actor-a", join.actor().id());
            assertEquals(source.lifecycleGeneration(), join.actor().targetNodeGeneration());
            assertEquals("target-room", join.targetSpot().id());
            assertEquals(target.lifecycleGeneration(),
                join.targetSpot().targetNodeGeneration());
            assertEquals("{\"join\":true}", new String(
                join.payload().payload(), StandardCharsets.UTF_8));
        }
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(peer -> peer.state()
                == systems.zlink.framework.runtime.internal.binding.spot
                    .MeshPeerState.ADMITTED)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(peer -> peer.state()
            == systems.zlink.framework.runtime.internal.binding.spot
                .MeshPeerState.ADMITTED));
    }
}
