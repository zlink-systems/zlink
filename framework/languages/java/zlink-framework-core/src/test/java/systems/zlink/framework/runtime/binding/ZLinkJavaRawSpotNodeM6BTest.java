package systems.zlink.framework.runtime.binding;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.core.Zlink;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

final class ZLinkJavaRawSpotNodeM6BTest {
    @Test
    void boundSessionSubmissionRetriesTransientBackpressure()
        throws Exception {
        AtomicInteger attempts = new AtomicInteger();

        ZLinkJavaStreamSocket.submitBoundSessionUntilAccepted(
                Duration.ofSeconds(1),
                () -> attempts.incrementAndGet() >= 2)
            .toCompletableFuture()
            .get(1, TimeUnit.SECONDS);

        assertEquals(2, attempts.get());
    }

    @Test
    void remoteUserSpotCreateAndCloseUseExactTerminalOperations()
        throws Exception {
        String endpoint =
            "inproc://jvm-m6b-user-spot-" + System.nanoTime();
        RoutingId sourceRid =
            RoutingId.from("jvm-m6b-user-spot-source");
        RoutingId targetRid =
            RoutingId.from("jvm-m6b-user-spot-target");
        String spotId =
            "jvm-m6b-user-spot-room";
        AtomicLong targetClock =
            new AtomicLong(System.currentTimeMillis());
        try (var context = Zlink.createContext();
             var target = new ZLinkJavaRawMeshNode(
                 context, "mesh", targetClock::get);
             var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            source.setRoutingId(sourceRid);
            source.setBind(
                "inproc://jvm-m6b-user-spot-source-"
                    + System.nanoTime());
            target.start();
            source.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            AtomicInteger createCalls = new AtomicInteger();
            AtomicInteger closeCalls = new AtomicInteger();
            target.setUserSpotOperationHandler(
                new ZLinkInternalMeshNode.UserSpotOperationHandler() {
                    @Override
                    public CompletionStage<
                        ZLinkInternalMeshNode.UserSpotCreateResponse>
                        create(
                            ZLinkInternalMeshNode.UserSpotCreateRequest
                                request) {
                        createCalls.incrementAndGet();
                        assertEquals(sourceRid, request.sourceNodeRid());
                        assertEquals(
                            spotId, request.intent().spotId());
                        return CompletableFuture.completedFuture(
                            new ZLinkInternalMeshNode
                                .UserSpotCreateResponse(
                                    ZLinkServiceM6BWireCodec
                                        .UserSpotCreateResult.CREATED,
                                    spotId,
                                    17,
                                    List.of(
                                        Message.from("reply-name"),
                                        Message.from("created"))));
                    }

                    @Override
                    public CompletionStage<
                        ZLinkInternalMeshNode.UserSpotCloseResponse>
                        close(
                            ZLinkInternalMeshNode.UserSpotCloseRequest
                                request) {
                        closeCalls.incrementAndGet();
                        assertEquals(
                            17,
                            request.intent().target()
                                .objectGeneration());
                        return CompletableFuture.completedFuture(
                            new ZLinkInternalMeshNode
                                .UserSpotCloseResponse(true));
                    }
                });

            long deadline = targetClock.get() + 2_000;
            var reservation =
                new ZLinkServiceM6BWireCodec.ReservationFence(
                    "reservation",
                    "store-1",
                    17,
                    19,
                    targetRid,
                    target.lifecycleGeneration(),
                    "owner-target",
                    23,
                    1);
            var intent =
                new ZLinkInternalMeshNode.UserSpotCreateIntent(
                    spotId,
                    "room-v1",
                    reservation,
                    deadline);
            var firstCreate = source.requestUserSpotCreate(
                targetRid,
                intent,
                Duration.ofSeconds(2),
                71,
                73);
            var created = firstCreate
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            var duplicateCreate = source.requestUserSpotCreate(
                targetRid,
                intent,
                Duration.ofSeconds(2),
                71,
                73);
            var replayed = duplicateCreate
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            targetClock.set(deadline + 200);
            var replayedAfterDeadline = source.requestUserSpotCreate(
                    targetRid,
                    intent,
                    Duration.ofSeconds(2),
                    71,
                    73)
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            try {
                assertEquals(
                    ZLinkServiceM6BWireCodec
                        .UserSpotCreateResult.CREATED,
                    created.result());
                assertEquals(spotId, created.spotId());
                assertEquals(17, created.objectGeneration());
                assertEquals(
                    "created",
                    created.applicationReply()
                        .getLast()
                        .toUtf8String());
                assertEquals(
                    "created",
                    replayed.applicationReply()
                        .getLast()
                        .toUtf8String());
                assertEquals(
                    "created",
                    replayedAfterDeadline.applicationReply()
                        .getLast()
                        .toUtf8String());
            } finally {
                created.applicationReply().forEach(Message::close);
                replayed.applicationReply().forEach(Message::close);
                replayedAfterDeadline.applicationReply()
                    .forEach(Message::close);
            }

            assertThrows(
                java.util.concurrent.ExecutionException.class,
                () -> source.requestUserSpotCreate(
                        targetRid,
                        intent,
                        Duration.ofSeconds(2),
                        72,
                        74)
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS));

            long closeDeadline = targetClock.get() + 2_000;
            var closed = source.requestUserSpotClose(
                    targetRid,
                    new ZLinkInternalMeshNode.UserSpotCloseIntent(
                        new ZLinkServiceM6BWireCodec
                            .UserSpotCloseFence(
                                spotId,
                                17,
                                targetRid,
                                target.lifecycleGeneration(),
                                19,
                                "store-2"),
                        closeDeadline),
                    Duration.ofSeconds(2))
                .toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            assertTrue(closed.closed());
            assertEquals(1, createCalls.get());
            assertEquals(1, closeCalls.get());
        }
    }

    @Test
    void spotSendRejectsStaleGenerationBeforeDispatch() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-source");
            RoutingId targetRid = RoutingId.from("jvm-m6b-target");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot source = node.spotNode().createSpot(sourceRid.toString());
            ZLinkBackendSpot target = node.spotNode().createSpot(targetRid.toString());

            try (Message stale = Message.from("stale")) {
                assertFalse(source.sendToSpot(
                    nodeRid,
                    targetRid.toString(),
                    target.lifecycleGeneration() + 1,
                    List.of(stale),
                    SendFlags.DONT_WAIT));
            }
            try (Message current = Message.from("current")) {
                assertTrue(source.sendToSpot(
                    nodeRid,
                    targetRid.toString(),
                    target.lifecycleGeneration(),
                    List.of(current),
                    SendFlags.DONT_WAIT));
            }
            try (var received = target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT)) {
                assertNotNull(received);
                assertEquals(
                    "current",
                    received.parts().getFirst().toUtf8String());
            }
        }
    }

    @Test
    void localSpotRouteRetainsWireContentType() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-content-type-node");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot source = node.spotNode().createSpot("source");
            ZLinkBackendSpot target = node.spotNode().createSpot("target");
            try (Message packet = Message.from("Payload");
                 Message payload = Message.from("value");
                 Message contentType = ZLinkChannelContentTypeFrame.encode(
                     "application/example")) {
                assertTrue(source.sendToSpot(
                    nodeRid,
                    target.spotId(),
                    target.lifecycleGeneration(),
                    List.of(packet, payload, contentType),
                    SendFlags.DONT_WAIT));
                try (var received = target.recvRoute(
                    ZLinkBackendRecvMode.DONT_WAIT)) {
                    assertNotNull(received);
                    assertEquals("application/example", received.contentType());
                }
            }
        }
    }

    @Test
    void localActorDispatchRetainsWireContentType() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-actor-content-type-node");
            node.setRoutingId(nodeRid);
            CompletableFuture<String> receivedContentType = new CompletableFuture<>();
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event() != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                try {
                    receivedContentType.complete(
                        info.actorMessages().getFirst().contentType());
                } finally {
                    info.actorMessages().forEach(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorReceived::close);
                }
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("actor-content-type", create);
            }
            try (Message packet = Message.from("Payload");
                 Message payload = Message.from("value");
                 Message contentType = ZLinkChannelContentTypeFrame.encode(
                     "application/example")) {
                assertTrue(node.spotNode().sendToActor(
                    actor,
                    List.of(packet, payload, contentType),
                    SendFlags.DONT_WAIT));
                assertEquals(
                    "application/example",
                    receivedContentType.get(1, TimeUnit.SECONDS));
            }
        }
    }

    private static void awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (node.peers().stream().noneMatch(
                peer -> peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED)
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertTrue(node.peers().stream().anyMatch(
            peer -> peer.state()
                == systems.zlink.framework.runtime.internal.binding.spot
                    .MeshPeerState.ADMITTED));
    }

    @Test
    void remoteSpotAndActorRejectStaleAuthorityOwnerGeneration() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-owner-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-owner-source");
            String spotId = "jvm-m6b-owner-spot";
            node.setRoutingId(nodeRid);
            ZLinkJavaRawSpotNode spots =
                (ZLinkJavaRawSpotNode) node.spotNode();
            ZLinkBackendSpot spot = spots.createSpot(spotId);
            spot.rememberSpotAuthority(
                nodeRid, spotId, spot.lifecycleGeneration(), 31, 1);
            var staleSpot = new ZLinkServiceM6BWireCodec.SpotMessage(
                false,
                0,
                null,
                sourceRid.toString(),
                new ZLinkServiceM6BWireCodec.SpotRouteFence(
                    spotId,
                    spot.lifecycleGeneration(),
                    nodeRid,
                    1,
                    32,
                    1));
            assertFalse(spots.enqueueRemoteSpot(
                sourceRid,
                staleSpot,
                new byte[0],
                List.of(),
                ignored -> { }));

            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = spots.createActor("owner-actor", create);
            }
            spots.rememberActorAuthority(actor, 41, 1);
            var staleActor = new ZLinkServiceM6BWireCodec.ActorMessage(
                false,
                0,
                null,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    1,
                    42,
                    1));
            assertFalse(spots.enqueueRemoteActor(
                sourceRid,
                staleActor,
                List.of(),
                ignored -> { }));
        }
    }

    @Test
    void staleActorRelayOwnsPartsAndAdmissionLeaseAtTheInfrastructureBoundary() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-relay-target");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-relay-source");
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-m6b-relay-target-" + System.nanoTime());
            node.start();
            ZLinkJavaRawSpotNode spots =
                (ZLinkJavaRawSpotNode) node.spotNode();
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef
                current;
            try (Message create = Message.from("create")) {
                current = spots.createActor("relay-actor", 2, create);
            }

            var stale = new ZLinkServiceM6BWireCodec.ActorMessage(
                false,
                0,
                null,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(nodeRid, "relay-actor", 1),
                    node.lifecycleGeneration(),
                    1,
                    1));
            ZLinkInboundDispatchBudget budget =
                new ZLinkInboundDispatchBudget(64);
            ZLinkInboundDispatchBudget.Lease lease = budget.track(7);
            AtomicBoolean callbackCalled = new AtomicBoolean();
            AtomicBoolean failureCalled = new AtomicBoolean();
            spots.setMessageFollowRelayHandler(
                (sourceNodeRid,
                    sourceNodeGeneration,
                    header,
                    acceptedJournalRecord,
                    parts,
                    contentType,
                    inboundDispatchLease,
                    reply,
                    failure) -> {
                    assertEquals(sourceRid, sourceNodeRid);
                    assertEquals(9, sourceNodeGeneration);
                    assertEquals(stale, header);
                    assertEquals("application/json", contentType);
                    assertEquals(2, parts.size());
                    assertNotNull(inboundDispatchLease);
                    callbackCalled.set(true);
                    parts.forEach(Message::close);
                    inboundDispatchLease.close();
                    return true;
                });

            List<Message> parts = List.of(
                Message.from(ZLinkStreamHeaderCodec.encode(
                    new ZLinkStreamHeader(
                        "RelayReq",
                        java.util.Map.of(),
                        java.util.Optional.empty()))),
                Message.from("payload"));
            boolean accepted = spots.enqueueRemoteActor(
                sourceRid,
                9,
                stale,
                new byte[0],
                parts,
                "application/json",
                lease,
                null,
                ignored -> failureCalled.set(true));

            assertTrue(accepted);
            assertTrue(callbackCalled.get());
            assertFalse(failureCalled.get());
            assertEquals(0, budget.snapshot().pendingPayloadBytes());
            assertEquals("relay-actor", current.actorId());
        }
    }

    @Test
    void spotRequestPublishesOnlyTheFirstTerminalReply() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-request-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-request-source");
            RoutingId targetRid = RoutingId.from("jvm-m6b-request-target");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot source = node.spotNode().createSpot(sourceRid.toString());
            ZLinkBackendSpot target = node.spotNode().createSpot(targetRid.toString());
            AtomicInteger callbackCount = new AtomicInteger();
            CompletableFuture<String> reply = new CompletableFuture<>();
            target.onDispatchEvent(info -> {
                if (info.event() != ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
                    return;
                }
                try (var received =
                         target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
                     Message first = Message.from("first");
                     Message late = Message.from("late")) {
                    received.reply(List.of(first));
                    received.reply(List.of(late));
                }
            });

            try (Message request = Message.from("request")) {
                assertTrue(source.requestToSpot(
                    nodeRid,
                    targetRid.toString(),
                    target.lifecycleGeneration(),
                    List.of(request),
                    received -> {
                        callbackCount.incrementAndGet();
                        try (received) {
                            reply.complete(
                                received.parts().getFirst().toUtf8String());
                        }
                    },
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(1)));
            }

            assertEquals("first", reply.get(1, TimeUnit.SECONDS));
            Thread.sleep(20);
            assertEquals(1, callbackCount.get());
        }
    }

    @Test
    void actorJoinCommitsMembershipBeforeLeaveLifecycle() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-join-node");
            RoutingId targetRid = RoutingId.from("jvm-m6b-join-target");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot target = node.spotNode().createSpot(targetRid.toString());
            target.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE) {
                    return;
                }
                var request =
                    target.recvActorJoin(ZLinkBackendRecvMode.DONT_WAIT);
                target.replyActorJoin(request, 0, List.of());
            });

            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("actor-1", create);
            }
            var joined = node.spotNode().joinActor(
                actor,
                nodeRid,
                targetRid.toString(),
                target.lifecycleGeneration(),
                List.of(),
                Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            assertEquals(ZLinkBackendRequestResult.OK, joined.result());
            assertEquals(targetRid.toString(), joined.joinedSpotId());
            assertEquals(2, joined.joinEpoch());

            node.spotNode().leaveActor(
                actor, targetRid.toString(), Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            var lifecycle =
                target.recvActorLifecycle(ZLinkBackendRecvMode.DONT_WAIT);
            assertNotNull(lifecycle);
            assertEquals(
                ZLinkBackendActorLifecycleEventKind.LEFT,
                lifecycle.kind());
            assertEquals(3, lifecycle.info().joinEpoch());
        }
    }

    @Test
    void unansweredSpotRequestTerminatesAtDeadline() throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-timeout-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-timeout-source");
            RoutingId targetRid = RoutingId.from("jvm-m6b-timeout-target");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot source = node.spotNode().createSpot(sourceRid.toString());
            ZLinkBackendSpot target = node.spotNode().createSpot(targetRid.toString());
            CompletableFuture<ZLinkBackendRequestResult> result =
                new CompletableFuture<>();

            try (Message request = Message.from("request")) {
                assertTrue(source.requestToSpot(
                    nodeRid,
                    targetRid.toString(),
                    target.lifecycleGeneration(),
                    List.of(request),
                    received -> {
                        try (received) {
                            result.complete(received.result());
                        }
                    },
                    SendFlags.DONT_WAIT,
                    Duration.ofMillis(20)));
            }

            assertEquals(
                ZLinkBackendRequestResult.TIMED_OUT,
                result.get(1, TimeUnit.SECONDS));
            try (var queued =
                     target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT)) {
                assertNotNull(queued);
            }
        }
    }

    @Test
    void remoteSpotSendAndRequestUseTheExactRouteFence() throws Exception {
        String endpoint = "inproc://jvm-m6b-remote-" + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6b-remote-left");
        RoutingId rightRid = RoutingId.from("jvm-m6b-remote-right");
        RoutingId targetRid = RoutingId.from("jvm-m6b-remote-target");
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind(
                "inproc://jvm-m6b-remote-right-" + System.nanoTime());
            left.start();
            right.start();
            acceptExactSource(
                left, rightRid, right.lifecycleGeneration());
            right.connectPeer(endpoint, leftRid);
            ZLinkBackendSpot source = right.spotNode().createSpot(
                "jvm-m6b-remote-source");
            ZLinkBackendSpot target = left.spotNode().createSpot(targetRid.toString());
            target.rememberSpotAuthority(
                leftRid,
                targetRid.toString(),
                target.lifecycleGeneration(),
                77,
                1);
            source.rememberSpotAuthority(
                leftRid,
                targetRid.toString(),
                target.lifecycleGeneration(),
                77,
                1);
            CompletableFuture<String> sent = new CompletableFuture<>();
            AtomicInteger handlerCalls = new AtomicInteger();
            target.onDispatchEvent(info -> {
                if (info.event() != ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
                    return;
                }
                try (var received =
                         target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT)) {
                    handlerCalls.incrementAndGet();
                    String value =
                        received.parts().getLast().toUtf8String();
                    if (received.requestSeq().isPresent()) {
                        try (Message reply = Message.from("remote-reply")) {
                            received.reply(List.of(reply));
                        }
                    } else {
                        sent.complete(value);
                    }
                }
            });

            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            boolean submitted = false;
            while (!submitted && System.nanoTime() < deadline) {
                try (Message message = Message.from("remote-send")) {
                    submitted = source.sendToSpot(
                        leftRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(message),
                        SendFlags.DONT_WAIT);
                }
                if (!submitted) {
                    Thread.sleep(1);
                }
            }
            assertTrue(submitted);
            assertEquals("remote-send", sent.get(2, TimeUnit.SECONDS));

            CompletableFuture<String> reply = new CompletableFuture<>();
            try (Message message = Message.from("remote-request")) {
                assertTrue(source.requestToSpot(
                    leftRid,
                    targetRid.toString(),
                    target.lifecycleGeneration(),
                    List.of(message),
                    received -> {
                        try (received) {
                            reply.complete(
                                received.parts().getFirst().toUtf8String());
                        }
                    },
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(2)));
            }
            assertEquals(
                "remote-reply",
                reply.get(2, TimeUnit.SECONDS));
            assertEquals(2, handlerCalls.get());

            left.setPeerAuthorityResolver(
                (meshName, candidateRid, candidateGeneration) ->
                    CompletableFuture.completedFuture(
                        candidateRid.equals(leftRid)
                            ? java.util.Optional.of(
                                new ZLinkInternalMeshNode
                                    .PeerAuthorityFence(
                                        leftRid,
                                        candidateGeneration,
                                        "target-owner",
                                        1))
                            : java.util.Optional.empty()));
            CompletableFuture<ZLinkBackendRequestResult> stale =
                new CompletableFuture<>();
            try (Message message = Message.from("stale-source")) {
                assertTrue(source.requestToSpot(
                    leftRid,
                    targetRid.toString(),
                    target.lifecycleGeneration(),
                    List.of(message),
                    received -> {
                        try (received) {
                            stale.complete(received.result());
                        }
                    },
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(2)));
            }
            assertEquals(
                ZLinkBackendRequestResult.CONFLICT,
                stale.get(2, TimeUnit.SECONDS));
            assertEquals(2, handlerCalls.get());
        }
    }

    @Test
    void localActorRequestRunsOnItsOwningSpotAndRepliesOnce()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-actor-node");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                var received = info.actorMessages().getFirst();
                try (Message reply = Message.from("actor-reply")) {
                    node.spotNode().replyActorNoBind(
                        received.actor(),
                        received.sourceNodeRid(),
                        received.sourceSessionRid(),
                        received.requestId(),
                        received.flags(),
                        List.of(reply));
                } finally {
                    info.actorMessages().forEach(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorReceived::close);
                }
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("actor-local", create);
            }

            List<Message> reply;
            try (Message request = Message.from("actor-request")) {
                reply = node.spotNode().requestToActor(
                    actor,
                    List.of(request),
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(1)).toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            }
            try {
                assertEquals(
                    "actor-reply",
                    reply.getFirst().toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            assertFalse(node.spotNode().sendToActor(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
                        nodeRid,
                        actor.actorId(),
                        actor.generation() + 1),
                List.of(),
                SendFlags.DONT_WAIT));
        }
    }

    @Test
    void acceptedActorRequestCanReplyAfterSourceOwnershipMoves()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid =
                RoutingId.from("jvm-m6b-actor-reply-after-move");
            node.setRoutingId(nodeRid);
            CompletableFuture<
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorReceived> accepted =
                new CompletableFuture<>();
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                accepted.complete(info.actorMessages().getFirst());
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef
                actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor(
                    "actor-reply-after-move", create);
            }

            CompletionStage<List<Message>> request;
            try (Message payload = Message.from("actor-request")) {
                request = node.spotNode().requestToActor(
                    actor,
                    List.of(payload),
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(2));
            }
            var received = accepted.get(1, TimeUnit.SECONDS);
            node.spotNode().destroyActor(actor, Duration.ofSeconds(1))
                .toCompletableFuture().get(1, TimeUnit.SECONDS);
            try (received;
                 Message reply = Message.from("reply-after-move")) {
                node.spotNode().replyActorNoBind(
                    received.actor(),
                    received.sourceNodeRid(),
                    received.sourceSessionRid(),
                    received.requestId(),
                    received.flags(),
                    List.of(reply));
            }

            List<Message> reply =
                request.toCompletableFuture().get(1, TimeUnit.SECONDS);
            try {
                assertEquals(
                    "reply-after-move",
                    reply.getFirst().toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void remoteActorRequestRunsOnTheCurrentOwningSpot() throws Exception {
        String endpoint = "inproc://jvm-m6b-actor-remote-"
            + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6b-actor-left");
        RoutingId rightRid = RoutingId.from("jvm-m6b-actor-right");
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind(
                "inproc://jvm-m6b-actor-right-" + System.nanoTime());
            left.start();
            right.start();
            acceptExactSource(
                left, rightRid, right.lifecycleGeneration());
            right.connectPeer(endpoint, leftRid);
            ZLinkBackendSpot entry = left.spotNode().entrySpot();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                var received = info.actorMessages().getFirst();
                try (Message reply = Message.from("remote-actor-reply")) {
                    left.spotNode().replyActorNoBind(
                        received.actor(),
                        received.sourceNodeRid(),
                        received.sourceSessionRid(),
                        received.requestId(),
                        received.flags(),
                        List.of(reply));
                } finally {
                    info.actorMessages().forEach(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorReceived::close);
                }
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = left.spotNode().createActor("actor-remote", create);
            }
            left.spotNode().rememberActorAuthority(actor, 89, 1);
            right.spotNode().rememberActorAuthority(
                actor, 89, 1);

            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (right.peers().stream().noneMatch(
                peer -> peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED)
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(right.peers().stream().anyMatch(
                peer -> peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED));

            List<Message> reply;
            try (Message request = Message.from("remote-actor-request")) {
                reply = right.spotNode().requestToActor(
                    actor,
                    List.of(request),
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(2)).toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }
            try {
                assertEquals(
                    "remote-actor-reply",
                    reply.getFirst().toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void logicalMulticastFansOutLocallyAndOncePerAdmittedRemoteNode()
        throws Exception {
        String endpoint = "inproc://jvm-m6b-multicast-" + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6b-multicast-left");
        RoutingId rightRid = RoutingId.from("jvm-m6b-multicast-right");
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            left.setObjectRole(
                systems.zlink.framework.locations.ZLinkMeshNodeObjectRole.SERVER);
            left.addChannel("events");
            left.setChannelWeight("events", 10_000);
            right.setRoutingId(rightRid);
            right.setBind(
                "inproc://jvm-m6b-multicast-right-" + System.nanoTime());
            right.addChannel("events");
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);
            ZLinkBackendSpot remote = left.spotNode().createSpot(
                "jvm-m6b-multicast-target");
            remote.setSubscription("orders");

            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (right.peers().stream().noneMatch(
                    peer -> peer.routingId().equals(leftRid)
                        && peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED)
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(right.peers().stream().anyMatch(
                peer -> peer.routingId().equals(leftRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED));
            assertEquals(
                leftRid,
                right.selectPlacementTarget().orElseThrow());

            var source = (ZLinkJavaRawSpot) right.spotNode().createSpot(
                "jvm-m6b-multicast-source");
            try (Message packet = Message.from("Packet");
                 Message payload = Message.from("multicast")) {
                right.publishLogicalMulticast(
                    source,
                    "events",
                    "orders",
                    new byte[] {7},
                    List.of(packet, payload));
            }

            systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage
                received = null;
            while (received == null && System.nanoTime() < deadline) {
                received = remote.subscribe(
                    ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    Thread.sleep(1);
                }
            }
            assertNotNull(received);
            try {
                assertEquals("events", received.channelName());
                assertEquals("orders", received.topic());
                assertEquals(
                    "multicast",
                    received.parts().getLast().toUtf8String());
                assertEquals(7, received.applicationMetadata()[0]);
            } finally {
                received.parts().forEach(Message::close);
            }

            left.setPlacementWeight(0);
            long placementUpdateDeadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            do {
                if (right.selectPlacementTarget().isPresent()) {
                    Thread.sleep(1);
                }
            } while (right.selectPlacementTarget().isPresent()
                && System.nanoTime() < placementUpdateDeadline);
            assertTrue(right.selectPlacementTarget().isEmpty());
        }
    }

    @Test
    void command39ActivatesOnlyTheRegisteredExactAuthorityFence()
        throws Exception {
        String endpoint = "inproc://jvm-m6b-instance-wire-"
            + System.nanoTime();
        RoutingId leftRid = RoutingId.from("jvm-m6b-instance-wire-left");
        RoutingId rightRid = RoutingId.from("jvm-m6b-instance-wire-right");
        String spotId = "jvm-m6b-instance-wire-spot";
        try (var context = Zlink.createContext();
             var left = new ZLinkJavaRawMeshNode(context, "mesh");
             var right = new ZLinkJavaRawMeshNode(context, "mesh")) {
            left.setRoutingId(leftRid);
            left.setBind(endpoint);
            right.setRoutingId(rightRid);
            right.setBind(
                "inproc://jvm-m6b-instance-wire-right-"
                    + System.nanoTime());
            left.start();
            right.start();
            right.connectPeer(endpoint, leftRid);

            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (right.peers().stream().noneMatch(
                    peer -> peer.routingId().equals(leftRid)
                        && peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED)
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(right.peers().stream().anyMatch(
                peer -> peer.routingId().equals(leftRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED));

            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) left.spotNode();
            target.registerInstanceSpotType("orders");
            var route = new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                leftRid,
                left.lifecycleGeneration(),
                spotId,
                41,
                "owner-a",
                17,
                9,
                "store-3");
            target.registerInstanceSpotAuthority("orders", route);

            try (Message packet = Message.from("Packet");
                 Message payload = Message.from("activate")) {
                assertTrue(right.sendInstanceSpot(
                    route,
                    "orders",
                    null,
                    new byte[] {3},
                    List.of(packet, payload)));
            }

            ZLinkBackendSpot activated = null;
            while (activated == null && System.nanoTime() < deadline) {
                activated = target.localSpot(spotId);
                if (activated == null) {
                    Thread.sleep(1);
                }
            }
            assertNotNull(activated);
            assertEquals(41, activated.lifecycleGeneration());
            var routeDeadline = System.nanoTime()
                + Duration.ofSeconds(2).toNanos();
            var received = activated.recvRoute(
                ZLinkBackendRecvMode.DONT_WAIT);
            while (received == null && System.nanoTime() < routeDeadline) {
                Thread.sleep(1);
                received = activated.recvRoute(
                    ZLinkBackendRecvMode.DONT_WAIT);
            }
            assertNotNull(received);
            var delivered = received;
            try (delivered) {
                assertEquals(
                    "activate",
                    delivered.parts().getLast().toUtf8String());
                assertEquals(3, delivered.applicationMetadata()[0]);
            }

            var stale = new ZLinkServiceM6BWireCodec.InstanceRouteFence(
                leftRid,
                route.targetNodeGeneration(),
                spotId,
                route.objectGeneration(),
                route.ownerId(),
                route.authorityOwnerGeneration() + 1,
                route.leaseGeneration(),
                route.storeVersion());
            try (Message packet = Message.from("Packet");
                 Message payload = Message.from("stale")) {
                assertTrue(right.sendInstanceSpot(
                    stale, "orders", null, new byte[0],
                    List.of(packet, payload)));
            }
            Thread.sleep(20);
            assertEquals(
                null,
                activated.recvRoute(ZLinkBackendRecvMode.DONT_WAIT));
        }
    }

    @Test
    void instanceAuthorityReplacementRequiresAForwardFence() {
        RoutingId nodeRid = RoutingId.from("jvm-m6b-instance-fence-node");
        var current = new ZLinkServiceM6BWireCodec.InstanceRouteFence(
            nodeRid,
            3,
            "instance-fence-spot",
            7,
            "owner-a",
            11,
            5,
            "41");
        var recreated = new ZLinkServiceM6BWireCodec.InstanceRouteFence(
            nodeRid,
            3,
            "instance-fence-spot",
            8,
            "owner-a",
            12,
            6,
            "42");
        var forged = new ZLinkServiceM6BWireCodec.InstanceRouteFence(
            nodeRid,
            3,
            "instance-fence-spot",
            7,
            "owner-a",
            12,
            5,
            "41");

        assertTrue(
            ZLinkJavaRawSpotNode.isNewerInstanceAuthorityFence(
                recreated, current));
        assertFalse(
            ZLinkJavaRawSpotNode.isNewerInstanceAuthorityFence(
                current, recreated));
        assertFalse(
            ZLinkJavaRawSpotNode.isNewerInstanceAuthorityFence(
                forged, current));
    }

    @Test
    void streamBindingDispatchesThroughFrameworkActorAuthority()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(), node)) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-stream-node");
            RoutingId sessionRid = RoutingId.from("jvm-m6b-session");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot entry = node.spotNode().entrySpot();
            CompletableFuture<RoutingId> delivered =
                new CompletableFuture<>();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                var received = info.actorMessages().getFirst();
                delivered.complete(received.sourceSessionRid());
                info.actorMessages().forEach(
                    systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorReceived::close);
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = node.spotNode().createActor("actor-stream", create);
            }

            stream.startSessionService();
            long firstBindingGeneration =
                node.bindingGenerationSeed();
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try (Message message = Message.from("stream-ingress")) {
                assertTrue(stream.sendBoundActor(
                    sessionRid,
                    actor.actorId(),
                    List.of(message),
                    SendFlags.DONT_WAIT));
            }
            assertEquals(
                sessionRid,
                delivered.get(1, TimeUnit.SECONDS));

            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try (Message stale = Message.from("stale-local-ingress")) {
                assertFalse(((ZLinkJavaRawSpotNode) node.spotNode())
                    .forwardBoundStreamSession(
                        actor,
                        sessionRid,
                        firstBindingGeneration,
                        2,
                        stream,
                        List.of(stale)));
            }
            try (Message current = Message.from("current-local-ingress")) {
                assertTrue(stream.sendBoundActor(
                    sessionRid,
                    actor.actorId(),
                    List.of(current),
                    SendFlags.DONT_WAIT));
            }

            stream.unbindActor(sessionRid, actor.actorId())
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            assertThrows(
                IllegalStateException.class,
                () -> stream.sendBoundActor(
                    sessionRid,
                    actor.actorId(),
                    List.of(),
                    SendFlags.DONT_WAIT));
        }
    }

    @Test
    void remoteBindingInstallationSignalsBoundSessionAdmissionKey()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-admission-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-admission-source");
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-m6b-admission-" + System.nanoTime());
            node.start();

            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) node.spotNode();
            systems.zlink.framework.runtime.internal.backend
                .ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = target.createActor("actor-admission", create);
            }
            target.rememberActorAuthority(actor, 41, 1);
            AtomicReference<ZLinkBackendAdmissionKey> ready =
                new AtomicReference<>();
            target.setAdmissionReadyHandler(ready::set);

            var route = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor,
                node.lifecycleGeneration(),
                41,
                1);
            assertTrue(target.acceptRemoteStreamBinding(
                sourceRid,
                7,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    1,
                    route,
                    RoutingId.from("jvm-m6b-admission-session"),
                    true,
                    3)));

            assertEquals(
                ZLinkBackendAdmissionKey.boundSession(
                    actor.nodeRid(), actor.actorId(), actor.generation()),
                ready.get());
        }
    }

    @Test
    void remoteBindingIdentityFencesOldOwnerEpochWithoutGlobalCounter()
        throws Exception {
        try (var context = Zlink.createContext();
             var actorNode = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId actorNodeRid =
                RoutingId.from("jvm-m6b-identity-actor-node");
            actorNode.setRoutingId(actorNodeRid);
            actorNode.setBind(
                "inproc://jvm-m6b-identity-" + System.nanoTime());
            actorNode.start();
            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) actorNode.spotNode();
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef
                actor;
            try (Message create = Message.from("create")) {
                actor = target.createActor("actor-owner-epoch", create);
            }
            target.rememberActorAuthority(actor, 41, 1);
            var route =
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    actorNode.lifecycleGeneration(),
                    41,
                    1);
            RoutingId ownerA = RoutingId.from("session-owner-a");
            RoutingId ownerB = RoutingId.from("session-owner-b");
            RoutingId sessionA = RoutingId.from("session-a");
            RoutingId sessionB = RoutingId.from("session-b");

            assertTrue(target.acceptRemoteStreamBinding(
                ownerA,
                90,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    1, route, sessionA, true, 50)));
            assertTrue(target.acceptRemoteStreamBinding(
                ownerB,
                1,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    2, route, sessionB, true, 1)));
            assertTrue(target.acceptRemoteStreamBinding(
                ownerA,
                90,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    3, route, sessionA, false, 50)));
            assertBoundIngressRejected(
                target, ownerA, 90, route, sessionA, 50, 1);
            assertBoundIngressAccepted(
                target, ownerB, 1, route, sessionB, 1, 1);

            assertTrue(target.acceptRemoteStreamBinding(
                ownerB,
                2,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    4, route, sessionB, true, 1)));
            assertTrue(target.acceptRemoteStreamBinding(
                ownerB,
                1,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    5, route, sessionB, false, 1)));
            assertBoundIngressRejected(
                target, ownerB, 1, route, sessionB, 1, 2);
            assertBoundIngressAccepted(
                target, ownerB, 2, route, sessionB, 1, 1);
        }
    }

    @Test
    void streamCloseWaitsForSlowUnbindWithinTheLifecycleBound()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(),
                 node,
                 null,
                 delayedUnbind(false, 100))) {
            node.setRoutingId(RoutingId.from("slow-unbind-node"));
            stream.startSessionService();
            RoutingId sessionRid = RoutingId.from("slow-unbind-session");
            var actor =
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
                        RoutingId.from("slow-unbind-actor-node"),
                        "slow-unbind-actor",
                        1);
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            long started = System.nanoTime();
            stream.close();
            long elapsedMillis = TimeUnit.NANOSECONDS.toMillis(
                System.nanoTime() - started);
            assertTrue(elapsedMillis >= 75);
            assertTrue(elapsedMillis < 500);
        }
    }

    @Test
    void streamCloseObservesFailedUnbindWithoutBlockingIndefinitely()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from("failed-unbind-node"));
            var stream = new ZLinkJavaStreamSocket(
                context.createStreamSocket(),
                node,
                null,
                delayedUnbind(true, 50));
            stream.startSessionService();
            RoutingId sessionRid =
                RoutingId.from("failed-unbind-session");
            var actor =
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
                        RoutingId.from("failed-unbind-actor-node"),
                        "failed-unbind-actor",
                        1);
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            long started = System.nanoTime();
            assertThrows(IllegalStateException.class, stream::close);
            long elapsedMillis = TimeUnit.NANOSECONDS.toMillis(
                System.nanoTime() - started);
            assertTrue(elapsedMillis >= 25);
            assertTrue(elapsedMillis < 500);
            stream.close();
        }
    }

    @Test
    void streamClosePreservesCleanupAndNativeCloseFailures()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var raw = context.createStreamSocket()) {
            node.setRoutingId(RoutingId.from("dual-close-node"));
            var stream = new ZLinkJavaStreamSocket(
                raw,
                node,
                null,
                delayedUnbind(true, 10),
                () -> {
                    raw.close();
                    throw new IllegalStateException(
                        "simulated native close failure");
                });
            stream.startSessionService();
            RoutingId sessionRid = RoutingId.from("dual-close-session");
            var actor =
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
                        RoutingId.from("dual-close-actor-node"),
                        "dual-close-actor",
                        1);
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            IllegalStateException failure = assertThrows(
                IllegalStateException.class,
                stream::close);
            assertEquals(
                "STREAM binding cleanup did not complete",
                failure.getMessage());
            assertEquals(
                "simulated unbind failure",
                failure.getCause().getMessage());
            assertEquals(1, failure.getSuppressed().length);
            assertEquals(
                "simulated native close failure",
                failure.getSuppressed()[0].getMessage());
        }
    }

    @Test
    void streamCloseAggregatesSynchronousErrorAndContinuesCleanup()
        throws Exception {
        AtomicInteger unbindAttempts = new AtomicInteger();
        AtomicBoolean nativeCloseAttempted = new AtomicBoolean();
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh");
             var raw = context.createStreamSocket()) {
            node.setRoutingId(RoutingId.from("error-close-node"));
            ZLinkJavaStreamSocket.BoundSessionLifecycle lifecycle =
                new ZLinkJavaStreamSocket.BoundSessionLifecycle() {
                    @Override
                    public CompletionStage<Void> bind(
                        RoutingId sessionRid,
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorRef actor,
                        long bindingGeneration,
                        Duration timeout) {
                        return CompletableFuture.completedFuture(null);
                    }

                    @Override
                    public CompletionStage<Void> unbind(
                        RoutingId sessionRid,
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorRef actor,
                        long bindingGeneration,
                        Duration timeout) {
                        if (unbindAttempts.incrementAndGet() == 1) {
                            throw new AssertionError(
                                "simulated synchronous unbind error");
                        }
                        return CompletableFuture.completedFuture(null);
                    }
                };
            var stream = new ZLinkJavaStreamSocket(
                raw,
                node,
                null,
                lifecycle,
                () -> {
                    nativeCloseAttempted.set(true);
                    raw.close();
                    throw new IllegalStateException(
                        "simulated native close after Error");
                });
            stream.startSessionService();
            RoutingId sessionRid = RoutingId.from("error-close-session");
            for (int index = 1; index <= 2; index++) {
                var actor =
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            RoutingId.from("error-close-actor-node"),
                            "error-close-actor-" + index,
                            1);
                stream.bindActor(sessionRid, actor)
                    .submit(Duration.ofSeconds(1)).toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            }

            IllegalStateException failure = assertThrows(
                IllegalStateException.class,
                stream::close);
            assertEquals(2, unbindAttempts.get());
            assertTrue(nativeCloseAttempted.get());
            assertTrue(failure.getCause() instanceof AssertionError);
            assertEquals(
                "simulated synchronous unbind error",
                failure.getCause().getMessage());
            assertEquals(1, failure.getSuppressed().length);
            assertEquals(
                "simulated native close after Error",
                failure.getSuppressed()[0].getMessage());
        }
    }

    private static ZLinkJavaStreamSocket.BoundSessionLifecycle
        delayedUnbind(boolean fail, long delayMillis) {
        return new ZLinkJavaStreamSocket.BoundSessionLifecycle() {
            @Override
            public CompletionStage<Void> bind(
                RoutingId sessionRid,
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef actor,
                long bindingGeneration,
                Duration timeout) {
                return CompletableFuture.completedFuture(null);
            }

            @Override
            public CompletionStage<Void> unbind(
                RoutingId sessionRid,
                systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef actor,
                long bindingGeneration,
                Duration timeout) {
                CompletableFuture<Void> result =
                    new CompletableFuture<>();
                CompletableFuture.delayedExecutor(
                    delayMillis,
                    TimeUnit.MILLISECONDS).execute(() -> {
                        if (fail) {
                            result.completeExceptionally(
                                new IllegalStateException(
                                    "simulated unbind failure"));
                        } else {
                            result.complete(null);
                        }
                    });
                return result;
            }
        };
    }

    private static void assertBoundIngressAccepted(
        ZLinkJavaRawSpotNode target,
        RoutingId ownerRid,
        long ownerGeneration,
        ZLinkServiceM6BWireCodec.ActorRouteFence route,
        RoutingId sessionRid,
        long bindingGeneration,
        long sequence) {
        int flags =
            systems.zlink.framework.runtime.protocol
                .ServiceWireConstants.FLAG_BOUND_SESSION
                | systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        Message payload = Message.from("bound-ingress");
        boolean accepted = target.enqueueRemoteActor(
                ownerRid,
                ownerGeneration,
                new ZLinkServiceM6BWireCodec.ActorMessage(
                    false,
                    flags,
                    null,
                    null,
                    route,
                    new ZLinkServiceM6BWireCodec.BoundSessionTail(
                        sessionRid,
                        bindingGeneration,
                        sequence)),
                List.of(payload),
                null);
        if (!accepted) {
            payload.close();
        }
        assertTrue(accepted);
    }

    private static void assertBoundIngressRejected(
        ZLinkJavaRawSpotNode target,
        RoutingId ownerRid,
        long ownerGeneration,
        ZLinkServiceM6BWireCodec.ActorRouteFence route,
        RoutingId sessionRid,
        long bindingGeneration,
        long sequence) {
        int flags =
            systems.zlink.framework.runtime.protocol
                .ServiceWireConstants.FLAG_BOUND_SESSION
                | systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
        try (Message payload = Message.from("stale-bound-ingress")) {
            assertFalse(target.enqueueRemoteActor(
                ownerRid,
                ownerGeneration,
                new ZLinkServiceM6BWireCodec.ActorMessage(
                    false,
                    flags,
                    null,
                    null,
                    route,
                    new ZLinkServiceM6BWireCodec.BoundSessionTail(
                        sessionRid,
                        bindingGeneration,
                        sequence)),
                List.of(payload),
                null));
        }
    }

    @Test
    void remoteBoundStreamUsesRawMeshAndExactGenerationFences()
        throws Exception {
        String endpoint = "inproc://jvm-m6b-bound-stream-"
            + System.nanoTime();
        RoutingId actorNodeRid =
            RoutingId.from("jvm-m6b-bound-actor-node");
        RoutingId sessionNodeRid =
            RoutingId.from("jvm-m6b-bound-session-node");
        RoutingId sessionRid =
            RoutingId.from("jvm-m6b-bound-session");
        CopyOnWriteArrayList<String> pushed =
            new CopyOnWriteArrayList<>();
        try (var context = Zlink.createContext();
             var actorNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var sessionNode = new ZLinkJavaRawMeshNode(context, "mesh");
             var stream = new ZLinkJavaStreamSocket(
                 context.createStreamSocket(),
                 sessionNode,
                 (rid, parts, flags) -> {
                     assertEquals(sessionRid, rid);
                     pushed.add(parts.getLast().toUtf8String());
                     return true;
                 })) {
            actorNode.setRoutingId(actorNodeRid);
            actorNode.setBind(endpoint);
            sessionNode.setRoutingId(sessionNodeRid);
            sessionNode.setBind(
                "inproc://jvm-m6b-bound-session-owner-"
                    + System.nanoTime());
            actorNode.start();
            sessionNode.start();
            acceptExactSource(
                actorNode,
                sessionNodeRid,
                sessionNode.lifecycleGeneration());
            sessionNode.connectPeer(endpoint, actorNodeRid);

            ZLinkBackendSpot entry = actorNode.spotNode().entrySpot();
            CompletableFuture<RoutingId> ingress =
                new CompletableFuture<>();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                var received = info.actorMessages().getFirst();
                ingress.complete(received.sourceSessionRid());
                info.actorMessages().forEach(
                    systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorReceived::close);
            });
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef
                actor;
            try (Message create = Message.from("create")) {
                actor = actorNode.spotNode().createActor(
                    "actor-bound-remote", create);
            }
            actorNode.spotNode().rememberActorAuthority(actor, 73, 1);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (sessionNode.peers().stream().noneMatch(
                    peer -> peer.routingId().equals(actorNodeRid)
                        && peer.state()
                            == systems.zlink.framework.runtime.internal.binding.spot
                                .MeshPeerState.ADMITTED)
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertTrue(sessionNode.peers().stream().anyMatch(
                peer -> peer.routingId().equals(actorNodeRid)
                    && peer.state()
                        == systems.zlink.framework.runtime.internal.binding.spot
                            .MeshPeerState.ADMITTED));

            stream.startSessionService();
            long firstBindingGeneration =
                sessionNode.bindingGenerationSeed();
            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try (Message message = Message.from("ingress")) {
                assertTrue(stream.sendBoundActor(
                    sessionRid,
                    actor.actorId(),
                    List.of(message),
                    SendFlags.DONT_WAIT));
            }
            assertEquals(sessionRid, ingress.get(1, TimeUnit.SECONDS));

            sessionNode.spotNode().rememberActorAuthority(actor, 74, 1);
            try (Message push = Message.from("push-one")) {
                assertTrue(actorNode.spotNode().sendActorBoundSession(
                    actor, List.of(push), SendFlags.DONT_WAIT));
            }
            while (pushed.isEmpty()
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertEquals(List.of("push-one"), pushed);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            stream.bindActor(sessionRid, actor)
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            ZLinkJavaRawSpotNode source =
                (ZLinkJavaRawSpotNode) sessionNode.spotNode();
            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) actorNode.spotNode();
            var currentRoute =
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    actorNode.lifecycleGeneration(),
                    73,
                    1);
            try (Message packet = Message.from("message");
                 Message payload = Message.from("stale")) {
                assertFalse(source.acceptBoundSessionPush(
                    actorNodeRid,
                    actorNode.lifecycleGeneration(),
                    new ZLinkServiceM6BWireCodec.BoundSessionSend(
                        currentRoute, firstBindingGeneration),
                    List.of(packet, payload)));
                assertFalse(source.acceptBoundSessionPush(
                    actorNodeRid,
                    actorNode.lifecycleGeneration() + 1,
                    new ZLinkServiceM6BWireCodec.BoundSessionSend(
                        currentRoute, firstBindingGeneration + 1),
                    List.of(packet, payload)));
                assertFalse(source.acceptBoundSessionPush(
                    actorNodeRid,
                    actorNode.lifecycleGeneration(),
                    new ZLinkServiceM6BWireCodec.BoundSessionSend(
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            new systems.zlink.framework.runtime.internal.backend
                                .ZLinkBackendActorRef(
                                    actorNodeRid,
                                    actor.actorId(),
                                    actor.generation() + 1),
                            actorNode.lifecycleGeneration(),
                            73,
                            1),
                        firstBindingGeneration + 1),
                    List.of(packet, payload)));
                assertFalse(source.acceptBoundSessionPush(
                    actorNodeRid,
                    actorNode.lifecycleGeneration(),
                    new ZLinkServiceM6BWireCodec.BoundSessionSend(
                        new ZLinkServiceM6BWireCodec.ActorRouteFence(
                            actor,
                            actorNode.lifecycleGeneration(),
                            74,
                            1),
                        firstBindingGeneration + 1),
                    List.of(packet, payload)));
            }

            int boundFlags =
                systems.zlink.framework.runtime.protocol
                    .ServiceWireConstants.FLAG_BOUND_SESSION
                    | systems.zlink.framework.runtime.protocol
                        .ServiceWireConstants.FLAG_SOURCE_SPOT_ID;
            try (Message payload = Message.from("stale-ingress")) {
                assertFalse(target.enqueueRemoteActor(
                    sessionNodeRid,
                    sessionNode.lifecycleGeneration(),
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                        false,
                        boundFlags,
                        null,
                        null,
                        currentRoute,
                        new ZLinkServiceM6BWireCodec.BoundSessionTail(
                            sessionRid,
                            firstBindingGeneration,
                            10)),
                    List.of(payload),
                    null));
                assertFalse(target.enqueueRemoteActor(
                    sessionNodeRid,
                    sessionNode.lifecycleGeneration() + 1,
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                        false,
                        boundFlags,
                        null,
                        null,
                        currentRoute,
                        new ZLinkServiceM6BWireCodec.BoundSessionTail(
                            sessionRid,
                            firstBindingGeneration + 1,
                            11)),
                    List.of(payload),
                    null));
            }

            assertTrue(target.acceptRemoteStreamBinding(
                sessionNodeRid,
                sessionNode.lifecycleGeneration(),
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    999,
                    currentRoute,
                    sessionRid,
                    false,
                    firstBindingGeneration)));
            try (Message push = Message.from("push-two")) {
                assertTrue(actorNode.spotNode().sendActorBoundSession(
                    actor, List.of(push), SendFlags.DONT_WAIT));
            }
            while (pushed.size() < 2
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertEquals(
                List.of("push-one", "push-two"), pushed);

            stream.unbindActor(sessionRid, actor.actorId())
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);
            try (Message late = Message.from("late")) {
                assertFalse(actorNode.spotNode().sendActorBoundSession(
                    actor, List.of(late), SendFlags.DONT_WAIT));
            }
            assertEquals(2, pushed.size());
        }
    }

    @Test
    void instanceSpotColdActivationJoinsAndPreservesStableType()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from("jvm-m6b-instance-node"));
            ZLinkJavaRawSpotNode spots =
                (ZLinkJavaRawSpotNode) node.spotNode();
            String spotId = "jvm-m6b-instance";
            spots.registerInstanceSpotType("alpha");

            var first = spots.activateInstanceSpot(spotId, null)
                .toCompletableFuture();
            var joined = spots.activateInstanceSpot(spotId, "alpha")
                .toCompletableFuture();
            assertTrue(first.get(1, TimeUnit.SECONDS).spot()
                == joined.get(1, TimeUnit.SECONDS).spot());
            long firstGeneration =
                first.get(1, TimeUnit.SECONDS).spot().lifecycleGeneration();

            spots.registerInstanceSpotType("beta");
            assertThrows(
                IllegalStateException.class,
                () -> spots.activateInstanceSpot(
                    spotId, "beta"));
            assertThrows(
                IllegalStateException.class,
                () -> spots.activateInstanceSpot(
                    "jvm-m6b-instance-ambiguous", null));

            assertTrue(spots.closeInstanceSpot(
                spotId, firstGeneration));
            var reactivated = spots.activateInstanceSpot(spotId, null)
                .toCompletableFuture().get(1, TimeUnit.SECONDS);
            assertEquals("alpha", reactivated.stableType());
            assertTrue(
                reactivated.spot().lifecycleGeneration()
                    > firstGeneration);
        }
    }

    private static void acceptExactSource(
        ZLinkJavaRawMeshNode target,
        RoutingId sourceRid,
        long sourceGeneration) {
        target.setPeerAuthorityResolver(
            (meshName, candidateRid, candidateGeneration) ->
                CompletableFuture.completedFuture(
                    candidateGeneration > 0
                        ? java.util.Optional.of(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                candidateRid,
                                candidateGeneration,
                                "test-source-owner",
                                1))
                        : java.util.Optional.empty()));
    }
}
