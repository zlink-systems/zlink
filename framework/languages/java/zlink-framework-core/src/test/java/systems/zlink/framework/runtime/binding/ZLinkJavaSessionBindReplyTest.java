package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;

import java.nio.ByteBuffer;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import java.util.function.Supplier;

final class ZLinkJavaSessionBindReplyTest {
    @Test
    @SuppressWarnings("unchecked")
    void successfulRemoteBindAndReplacementCarryAcceptedGenerations() throws Exception {
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            var sourceRid = RoutingId.from("tail-source");
            var targetRid = RoutingId.from("tail-target");
            String endpoint = "inproc://bind-tail-" + System.nanoTime();
            source.setRoutingId(sourceRid);
            source.setBind(endpoint + "-source");
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    Optional.of(
                                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                    rid, generation, "owner", 1))));
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            var spots = (ZLinkJavaRawSpotNode) target.spotNode();
            spots.setRemoteSessionBindingOwner(
                    (node, actor, transition) ->
                            CompletableFuture.completedFuture(
                                    transition.apply(
                                            ZLinkInternalSpotNode.RemoteSessionBindingTransition
                                                    .NONE)));
            try (var create = Message.from("create")) {
                var actor = spots.createActor("tail-actor", create);
                spots.rememberActorAuthority(actor, 41, 1);
                var codec = new ZLinkServiceM6BWireCodec();
                var request =
                        ZLinkJavaRawMeshNode.class.getDeclaredMethod(
                                "requestDurable", RoutingId.class, Supplier.class, Duration.class);
                request.setAccessible(true);
                for (long generation : new long[] {7, 8}) {
                    var bind =
                            new ZLinkServiceM6BWireCodec.BoundSessionBind(
                                    generation,
                                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                            actor, target.lifecycleGeneration(), 41, 1),
                                    RoutingId.from("tail-session-" + generation),
                                    true,
                                    generation);
                    Supplier<List<byte[]>> prepare =
                            () -> List.of(codec.encodeBoundSessionBindHeader(bind));
                    var pending =
                            (CompletionStage<List<byte[]>>)
                                    request.invoke(
                                            source, targetRid, prepare, Duration.ofSeconds(2));
                    var frames = pending.toCompletableFuture().get(3, TimeUnit.SECONDS);
                    assertEquals(1, frames.size());
                    assertEquals(37, frames.getFirst().length);
                    var tail = ByteBuffer.wrap(frames.getFirst(), 21, 16);
                    assertEquals(generation, tail.getLong());
                    assertEquals(41, tail.getLong());
                }
            }
        }
    }
}
