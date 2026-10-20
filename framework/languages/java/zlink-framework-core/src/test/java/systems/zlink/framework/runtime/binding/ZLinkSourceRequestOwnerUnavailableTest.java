package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.OptionalLong;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.TimeUnit;

/**
 * Source request owner contract, raw node half: when the admitted sender has no live MeshNode
 * descriptor (fixture cases noDescriptor and expiredLease) the first accepting owner ends the Actor
 * or Spot request with the public Unavailable result (Session·Actor binding §4, Location runtime
 * §9.3, error model §2.1 representative code).
 */
final class ZLinkSourceRequestOwnerUnavailableTest {
    @Test
    void sourceRequestOwnerMissingDescriptorEndsActorRequestUnavailable() throws Exception {
        assertUnavailable(true);
    }

    @Test
    void sourceRequestOwnerMissingDescriptorEndsSpotRequestUnavailable() throws Exception {
        assertUnavailable(false);
    }

    private static void assertUnavailable(boolean actorRequest) throws Exception {
        RoutingId sourceRid = RoutingId.from("source-request-owner-source");
        RoutingId targetRid = RoutingId.from("source-request-owner-target");
        String endpoint = "inproc://source-request-owner-" + System.nanoTime();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setApplicationJobQueue(queue());
            target.setApplicationJobQueue(queue());
            source.setRoutingId(sourceRid);
            source.setBind("inproc://source-request-owner-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    rid.equals(targetRid)
                                            ? Optional.of(
                                                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                            rid, generation, "target-owner", 1))
                                            : Optional.empty()));
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            ZLinkJavaRawMeshNodeM6ATest.awaitAdmitted(source);
            try (var payload = Message.from("request")) {
                if (actorRequest) {
                    var actor = new ZLinkBackendActorRef(targetRid, "source-owner-actor", 1);
                    source.spotNode().rememberActorAuthority(actor, 1, 1);
                    CompletableFuture<?> request =
                            source.requestActor(
                                            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                                    actor, target.lifecycleGeneration(), 1, 1),
                                            List.of(payload),
                                            Duration.ofSeconds(5),
                                            null)
                                    .toCompletableFuture();
                    ExecutionException failure =
                            assertThrows(
                                    ExecutionException.class,
                                    () -> request.get(5, TimeUnit.SECONDS));
                    assertEquals(
                            ZLinkFrameworkErrorKind.UNAVAILABLE,
                            assertInstanceOf(ZLinkFrameworkException.class, failure.getCause())
                                    .kind());
                } else {
                    ((ZLinkJavaRawSpotNode) source.spotNode())
                            .rememberSpotAuthority(targetRid, "source-owner-spot", 1, 1, 1);
                    CompletableFuture<?> request =
                            source.requestSpot(
                                            "source-spot",
                                            targetRid,
                                            "source-owner-spot",
                                            1,
                                            new byte[0],
                                            List.of(payload),
                                            Duration.ofSeconds(5),
                                            null)
                                    .toCompletableFuture();
                    try (var reply = (ZLinkBackendReceived) request.get(5, TimeUnit.SECONDS)) {
                        assertEquals(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                reply.result().toFrameworkErrorKind(reply.failureCode()));
                        assertEquals(
                                (int) ServiceWireConstants.FRAMEWORK_ERROR_ROUTE_NOT_CONNECTED,
                                reply.failureCode());
                    }
                }
            }
        }
    }

    private static ZLinkApplicationJobQueue queue() {
        return new ZLinkApplicationJobQueue(
                ZLinkApplicationJobQueueProfile.BALANCED,
                OptionalLong.empty(),
                new ZLinkApplicationJobQueue.ProcessorCandidates(1, null, null, null));
    }
}
