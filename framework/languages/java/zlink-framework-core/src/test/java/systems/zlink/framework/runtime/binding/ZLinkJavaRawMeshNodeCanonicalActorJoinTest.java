package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class ZLinkJavaRawMeshNodeCanonicalActorJoinTest {
    @Test
    void localActorLeftReportsAllCleanupFailuresOnceWithoutResubmission() {
        AtomicReference<Throwable> reportedFailure = new AtomicReference<>();
        AtomicInteger reports = new AtomicInteger();
        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger completed = new AtomicInteger();
        Logger logger = Logger.getLogger(ZLinkJavaRawMeshNode.class.getName());
        Handler diagnostics =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        if (record.getMessage().startsWith("Actor Left handler failed:")) {
                            reportedFailure.set(record.getThrown());
                            reports.incrementAndGet();
                        }
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(diagnostics);
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId node = RoutingId.from("r4-java-source-cleanup");
            source.setRoutingId(node);
            source.setActorLeftHandler(
                    (rid, left) -> {
                        attempts.incrementAndGet();
                        return ZLinkHandlerStages.completeAll(
                                List.of(
                                        () ->
                                                CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "first source cleanup failure")),
                                        () ->
                                                CompletableFuture.failedFuture(
                                                        new IllegalStateException(
                                                                "second source cleanup failure")),
                                        () -> {
                                            completed.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        }));
                    });
            source.sendActorLeft(
                            node,
                            new ZLinkServiceM6BWireCodec.ActorLeft(
                                    new ZLinkServiceM6BWireCodec.ActorIdentity("actor-r4", 7L),
                                    "source-spot",
                                    1L,
                                    2L))
                    .toCompletableFuture()
                    .join();
            assertEquals(1, reports.get());
            assertEquals(1, attempts.get());
            assertEquals(1, completed.get());
            assertEquals("first source cleanup failure", reportedFailure.get().getMessage());
            assertEquals(1, reportedFailure.get().getSuppressed().length);
            assertEquals(
                    "second source cleanup failure",
                    reportedFailure.get().getSuppressed()[0].getMessage());
        } finally {
            logger.removeHandler(diagnostics);
        }
    }

    @Test
    void actorJoin28UsesStructuralFlavorSelectionAndReturnsTypedReplies() throws Exception {
        String endpoint = "inproc://jvm-canonical-actor-join-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-canonical-source");
        RoutingId targetRid = RoutingId.from("jvm-canonical-target");
        AtomicReference<ServiceWirePilotCodec.ActorJoin28> received = new AtomicReference<>();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-canonical-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    generation == 0
                                            ? Optional.empty()
                                            : Optional.of(
                                                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                            rid, generation, "source-owner", 1L))));
            target.setCanonicalActorJoinHandler(
                    (sourcePeer, join) -> {
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

            ((ZLinkJavaRawSpotNode) source.spotNode())
                    .rememberSpotAuthority(targetRid, "target-room", 5L, 9L, 11L);
            var request =
                    new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                            new ZLinkBackendActorRef(sourceRid, "actor-a", 3L),
                            source.lifecycleGeneration(),
                            1L,
                            2L,
                            targetRid,
                            target.lifecycleGeneration(),
                            "target-room",
                            5L,
                            9L,
                            11L,
                            false,
                            "ZLinkFrameworkActorJoinRequest",
                            "application/json",
                            "{\"transferId\":\"canonical-text\"}".getBytes(StandardCharsets.UTF_8));

            assertTrue(source.canRequestCanonicalActorJoin(request));
            var reply =
                    source.requestCanonicalActorJoin(request, Duration.ofSeconds(1))
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS);

            assertTrue(reply.accepted());
            assertEquals(
                    32_768L,
                    reply.receiveChunkLimitBytes(),
                    "receiver owns the canonical admission reply chunk limit");
            assertEquals("target-reply", reply.applicationReply().getFirst().toUtf8String());
            reply.applicationReply().forEach(Message::close);
            ServiceWirePilotCodec.ActorJoin28 join = received.get();
            assertEquals("actor-a", join.actor().id());
            assertEquals(source.lifecycleGeneration(), join.actor().targetNodeGeneration());
            assertEquals("target-room", join.targetSpot().id());
            assertEquals(target.lifecycleGeneration(), join.targetSpot().targetNodeGeneration());
            assertEquals(
                    "{\"transferId\":\"canonical-text\"}",
                    new String(join.payload().payload(), StandardCharsets.UTF_8));
        }
    }

    @Test
    void actorJoin28LeavesTheSpotFenceJudgementToTheReceivingOwner() throws Exception {
        String endpoint = "inproc://jvm-canonical-actor-join-fence-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-canonical-fence-source");
        RoutingId targetRid = RoutingId.from("jvm-canonical-fence-target");
        AtomicReference<ServiceWirePilotCodec.ActorJoin28> received = new AtomicReference<>();
        try (var context = Zlink.createContext();
                var source = new ZLinkJavaRawMeshNode(context, "mesh");
                var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-canonical-fence-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setPeerAuthorityResolver(
                    (mesh, rid, generation) ->
                            CompletableFuture.completedFuture(
                                    generation == 0
                                            ? Optional.empty()
                                            : Optional.of(
                                                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                                                            rid, generation, "source-owner", 1L))));
            target.setCanonicalActorJoinHandler(
                    (sourcePeer, join) -> {
                        received.set(join);
                        return CompletableFuture.completedFuture(
                                new ZLinkInternalMeshNode.CanonicalActorJoinResponse(
                                        false, 7L, List.of()));
                    });
            source.start();
            target.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);
            //  The requester observed an older Spot authority than the fence it sends.
            ((ZLinkJavaRawSpotNode) source.spotNode())
                    .rememberSpotAuthority(targetRid, "target-room", 5L, 9L, 11L);
            var request =
                    new ZLinkInternalMeshNode.CanonicalActorJoinRequest(
                            new ZLinkBackendActorRef(sourceRid, "actor-a", 3L),
                            source.lifecycleGeneration(),
                            1L,
                            2L,
                            targetRid,
                            target.lifecycleGeneration(),
                            "target-room",
                            5L,
                            10L,
                            12L,
                            false,
                            "ZLinkFrameworkActorJoinRequest",
                            "application/json",
                            "{}".getBytes(StandardCharsets.UTF_8));

            var reply =
                    source.requestCanonicalActorJoin(request, Duration.ofSeconds(1))
                            .toCompletableFuture()
                            .get(1, TimeUnit.SECONDS);

            assertFalse(reply.accepted(), "the receiving owner returns its own admission result");
            ServiceWirePilotCodec.ActorJoin28 join = received.get();
            assertEquals(10L, join.targetSpot().expectedAuthorityOwnerGeneration());
            assertEquals(12L, join.targetSpot().expectedOwnerLeaseGeneration());
        }
    }

    @Test
    void supersededActorJoinMapsToInvalidOperation() {
        // Error model §2.1 sends InvalidOperation without a cause as invalidState/none.
        assertArrayEquals(
                new int[] {111, 0},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                "attempt was superseded")));
        assertArrayEquals(
                new int[] {105, 13},
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.UNAVAILABLE,
                                "route or store is unavailable")));
        assertEquals(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                        .INTERNAL_ERROR
                        .toFrameworkErrorKind(13));
        int[] unrelatedInvalidOperation =
                ZLinkJavaRawMeshNode.canonicalActorJoinFailurePair(
                        new ZLinkFrameworkException(
                                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                                "an unrelated invalid operation"));
        assertArrayEquals(new int[] {111, 0}, unrelatedInvalidOperation);
        assertTrue(
                ServiceWireConstants.validTerminalFailure(
                        unrelatedInvalidOperation[0], unrelatedInvalidOperation[1]));
        assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult
                        .fromWireTerminal(unrelatedInvalidOperation[0])
                        .toFrameworkErrorKind(unrelatedInvalidOperation[1]));
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream()
                        .noneMatch(
                                peer ->
                                        peer.state()
                                                == systems.zlink.framework.runtime.internal.binding
                                                        .spot.MeshPeerState.ADMITTED)
                && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(
                node.peers().stream()
                        .anyMatch(
                                peer ->
                                        peer.state()
                                                == systems.zlink.framework.runtime.internal.binding
                                                        .spot.MeshPeerState.ADMITTED));
    }
}
