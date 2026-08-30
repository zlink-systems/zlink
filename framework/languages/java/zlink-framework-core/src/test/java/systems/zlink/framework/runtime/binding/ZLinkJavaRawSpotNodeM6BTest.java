package systems.zlink.framework.runtime.binding;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.ExecutionException;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerEntry;
import systems.zlink.framework.runtime.internal.binding.spot.MeshPeerState;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
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
import systems.zlink.contracts.errors.ZlinkRequestException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.RequestResult;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEventKind;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceLivenessRegistry;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;

final class ZLinkJavaRawSpotNodeM6BTest {
    @Test
    void boundSessionLivenessSnapshotAllowsPreviouslyReadyValidationOnly() {
        assertTrue(ZLinkJavaRawMeshNode.boundSessionLivenessAllows(
            ZLinkServiceLivenessRegistry.Readiness.READY));
        assertTrue(ZLinkJavaRawMeshNode.boundSessionLivenessAllows(
            ZLinkServiceLivenessRegistry.Readiness
                .VALIDATING_PREVIOUSLY_READY));
        assertFalse(ZLinkJavaRawMeshNode.boundSessionLivenessAllows(
            ZLinkServiceLivenessRegistry.Readiness.NOT_READY));
    }

    @Test
    void serviceDescriptorStaysPreparingUntilHostMarksReady()
        throws Exception {
        RoutingId targetRid = RoutingId.from("jvm-ready-target");
        RoutingId sourceRid = RoutingId.from("jvm-ready-source");
        String endpoint = "inproc://jvm-service-ready-" + System.nanoTime();
        try (var context = Zlink.createContext();
             var target = new ZLinkJavaRawMeshNode(context, "mesh");
             var source = new ZLinkJavaRawMeshNode(context, "mesh")) {
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            target.setObjectRole(
                ZLinkMeshNodeObjectRole.SERVER);
            target.deferServiceReadyPublication();
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-service-ready-source-"
                + System.nanoTime());

            target.start();
            source.start();
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);
            assertTrue(source.selectPlacementTarget().isEmpty());

            target.markServiceReady();
            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            Optional<RoutingId> placement = source.selectPlacementTarget();
            while (placement.isEmpty() && System.nanoTime() < deadline) {
                Thread.sleep(1);
                placement = source.selectPlacementTarget();
            }
            assertEquals(targetRid, placement.orElseThrow());
        }
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
                ExecutionException.class,
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
    void spotSendUsesCurrentReadySpotAcrossGenerationChange() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-source");
            RoutingId targetRid = RoutingId.from("jvm-m6b-target");
            node.setRoutingId(nodeRid);
            ZLinkBackendSpot source = node.spotNode().createSpot(sourceRid.toString());
            ZLinkBackendSpot target = node.spotNode().createSpot(targetRid.toString());

            try (Message stale = Message.from("stale-address")) {
                source.sendToSpot(
                        nodeRid,
                        targetRid.toString(),
                        target.lifecycleGeneration() + 1,
                        List.of(stale))
                    .toCompletableFuture()
                    .join();
            }
            try (Message current = Message.from("current")) {
                source.sendToSpot(
                        nodeRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(current))
                    .toCompletableFuture()
                    .join();
            }
            try (var received = target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT)) {
                assertNotNull(received);
                assertEquals(
                    "stale-address",
                    received.parts().getFirst().toUtf8String());
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
                source.sendToSpot(
                        nodeRid,
                        target.spotId(),
                        target.lifecycleGeneration(),
                        List.of(packet, payload, contentType))
                    .toCompletableFuture()
                    .join();
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
            ZLinkBackendActorRef actor;
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

    private static MeshPeerEntry awaitAdmitted(ZLinkJavaRawMeshNode node)
        throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            Optional<MeshPeerEntry> matched = node.peers().stream()
                .filter(peer -> peer.state() == MeshPeerState.ADMITTED)
                .findFirst();
            if (matched.isPresent()) {
                return matched.orElseThrow();
            }
            Thread.sleep(1);
        }
        throw new AssertionError("ADMITTED peer was not observed");
    }

    private static MeshPeerEntry awaitAdmitted(
        ZLinkJavaRawMeshNode node,
        RoutingId peerRoutingId) throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            Optional<MeshPeerEntry> matched = node.peers().stream()
                .filter(peer -> peer.routingId().equals(peerRoutingId)
                    && peer.state() == MeshPeerState.ADMITTED)
                .findFirst();
            if (matched.isPresent()) {
                return matched.orElseThrow();
            }
            Thread.sleep(1);
        }
        throw new AssertionError(
            "ADMITTED peer was not observed: " + peerRoutingId);
    }

    private static RoutingId awaitPlacementTarget(
        ZLinkJavaRawMeshNode node) throws InterruptedException {
        long deadline =
            System.nanoTime() + Duration.ofSeconds(2).toNanos();
        Optional<RoutingId> placement = node.selectPlacementTarget();
        while (placement.isEmpty() && System.nanoTime() < deadline) {
            Thread.sleep(1);
            placement = node.selectPlacementTarget();
        }
        return placement.orElseThrow();
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

            ZLinkBackendActorRef actor;
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
    void exactRelocationForwardKeepsRawActorRequestContextUntilTargetReplay()
        throws Exception {
        String endpoint = "inproc://jvm-relocation-forward-"
            + System.nanoTime();
        String sourceEndpoint = "inproc://jvm-relocation-source-"
            + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-relocation-source");
        RoutingId targetRid = RoutingId.from("jvm-relocation-target");
        RoutingId callerRid = RoutingId.from("jvm-relocation-caller");
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh");
             var caller = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind(sourceEndpoint);
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            caller.setRoutingId(callerRid);
            caller.setBind("inproc://jvm-relocation-caller-"
                + System.nanoTime());
            source.start();
            target.start();
            caller.start();
            acceptExactSource(
                target, sourceRid, source.lifecycleGeneration());
            acceptExactSource(
                source, callerRid, caller.lifecycleGeneration());
            source.connectPeer(endpoint, targetRid);
            caller.connectPeer(sourceEndpoint, sourceRid);
            awaitAdmitted(source);
            awaitAdmitted(caller);

            var sourceRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(sourceRid, "moving-actor", 7),
                source.lifecycleGeneration(),
                11,
                3);
            var targetRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(targetRid, "moving-actor", 7),
                target.lifecycleGeneration(),
                12,
                5);
            installActorFollow(source.spotNode(), sourceRoute, targetRoute);
            var sourceSpotRoute = new ZLinkServiceM6BWireCodec.SpotRouteFence(
                "moving-spot",
                17,
                sourceRid,
                source.lifecycleGeneration(),
                21,
                3);
            var targetSpotRoute = new ZLinkServiceM6BWireCodec.SpotRouteFence(
                "moving-spot",
                17,
                targetRid,
                target.lifecycleGeneration(),
                22,
                5);
            source.spotNode().installRelocationSpotForward(
                sourceSpotRoute, targetSpotRoute, Duration.ofMinutes(1));

            List<String> targetFifo = new CopyOnWriteArrayList<>();
            List<String> targetSpotFifo = new CopyOnWriteArrayList<>();
            AtomicReference<Throwable> targetFailure = new AtomicReference<>();
            AtomicReference<java.util.function.Consumer<List<Message>>>
                targetRequestReply = new AtomicReference<>();
            CompletableFuture<Void> receivedAll = new CompletableFuture<>();
            CompletableFuture<Void> receivedSpots = new CompletableFuture<>();
            target.spotNode().setRelocationStagingIngressHandler(
                new ZLinkInternalSpotNode.RelocationStagingIngressHandler() {
                    @Override
                    public boolean handleSpot(
                        ZLinkInternalMeshNode.PeerAuthorityFence peer,
                        ZLinkServiceM6BWireCodec.SpotMessage header,
                        byte[] metadata,
                        java.util.function.Supplier<byte[]> acceptedRecord,
                        int acceptedRecordSizeHint,
                        List<Message> parts,
                        String contentType,
                        java.util.function.Consumer<List<Message>> reply,
                        java.util.function.Consumer<Throwable> failure) {
                        try {
                            assertEquals(sourceRid, peer.sourceNodeRid());
                            assertEquals(targetSpotRoute, header.target());
                            assertTrue(acceptedRecord.get().length > 0);
                            targetSpotFifo.add(parts.getLast().toUtf8String());
                            if (targetSpotFifo.size() == 2) {
                                receivedSpots.complete(null);
                            }
                        } catch (Throwable error) {
                            targetFailure.compareAndSet(null, error);
                            receivedSpots.completeExceptionally(error);
                        } finally {
                            parts.forEach(Message::close);
                        }
                        return true;
                    }

                    @Override
                    public boolean handleActor(
                        ZLinkInternalMeshNode.PeerAuthorityFence peer,
                        ZLinkServiceM6BWireCodec.ActorMessage header,
                        java.util.function.Supplier<byte[]> acceptedRecord,
                        List<Message> parts,
                        String contentType,
                        java.util.function.Consumer<List<Message>> reply,
                        java.util.function.Consumer<Throwable> failure) {
                        try {
                            assertEquals(sourceRid, peer.sourceNodeRid());
                            assertEquals(source.lifecycleGeneration(),
                                peer.sourceNodeGeneration());
                            assertEquals(targetRoute, header.target());
                            assertTrue(acceptedRecord.get().length > 0);
                            targetFifo.add(parts.getLast().toUtf8String());
                            if (header.request()) {
                                assertTrue(targetRequestReply.compareAndSet(
                                    null, reply));
                            }
                            if (targetFifo.size() == 4) {
                                receivedAll.complete(null);
                            }
                        } catch (Throwable error) {
                            targetFailure.compareAndSet(null, error);
                            receivedAll.completeExceptionally(error);
                        } finally {
                            parts.forEach(Message::close);
                        }
                        return true;
                    }
                });

            AtomicInteger callerFailures = new AtomicInteger();
            ZLinkJavaRawSpotNode sourceSpots =
                (ZLinkJavaRawSpotNode) source.spotNode();
            caller.spotNode().rememberActorAuthority(
                sourceRoute.actor(),
                sourceRoute.authorityOwnerGeneration(),
                sourceRoute.ownerLeaseGeneration());
            for (int index = 0; index < 3; index++) {
                List<Message> parts = List.of(
                    Message.from(ZLinkStreamHeaderCodec.encode(
                        new ZLinkStreamHeader(
                            "RelocatedPacket",
                            Map.of(),
                            Optional.empty()))),
                    Message.from("suffix-" + index));
                try {
                    caller.spotNode().sendToActorAsync(
                            sourceRoute.actor(), parts)
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS);
                } finally {
                    parts.forEach(Message::close);
                }
            }

            CompletionStage<List<Message>> pendingReply;
            List<Message> requestParts = List.of(
                Message.from(ZLinkStreamHeaderCodec.encode(
                    new ZLinkStreamHeader(
                        "RelocatedRequest",
                        Map.of(),
                        Optional.empty()))),
                Message.from("request-suffix"));
            pendingReply = caller.spotNode().requestToActor(
                sourceRoute.actor(),
                requestParts,
                SendFlags.DONT_WAIT,
                Duration.ofSeconds(3));
            requestParts.forEach(Message::close);
            receivedAll.get(3, TimeUnit.SECONDS);
            assertFalse(pendingReply.toCompletableFuture().isDone(),
                "the source must retain the reply route while target ingress is staged");
            targetRequestReply.get().accept(
                List.of(Message.from("target-staged-reply")));
            List<Message> forwardedReply = pendingReply.toCompletableFuture()
                .get(3, TimeUnit.SECONDS);
            try {
                assertEquals("target-staged-reply",
                    forwardedReply.getFirst().toUtf8String());
            } finally {
                forwardedReply.forEach(Message::close);
            }
            for (int index = 0; index < 2; index++) {
                var header = new ZLinkServiceM6BWireCodec.SpotMessage(
                    false,
                    0,
                    null,
                    201,
                    index + 1,
                    0,
                    "caller-spot",
                    sourceSpotRoute);
                List<Message> parts = List.of(
                    Message.from(ZLinkStreamHeaderCodec.encode(
                        new ZLinkStreamHeader(
                            "RelocatedSpotPacket",
                            Map.of(),
                            Optional.empty()))),
                    Message.from("spot-suffix-" + index));
                boolean accepted = sourceSpots.enqueueRemoteSpotLazy(
                    new ZLinkInternalMeshNode.PeerAuthorityFence(
                        callerRid, 9, "caller-owner", 7),
                    header,
                    new byte[0],
                    () -> new byte[0],
                    0,
                    parts,
                    null,
                    null,
                    ignored -> callerFailures.incrementAndGet());
                if (!accepted) {
                    parts.forEach(Message::close);
                }
                assertTrue(accepted,
                    "the exact old Spot route must reroute without rejection");
            }
            receivedSpots.get(3, TimeUnit.SECONDS);
            assertEquals(List.of(
                    "suffix-0", "suffix-1", "suffix-2", "request-suffix"),
                targetFifo);
            assertEquals(List.of("spot-suffix-0", "spot-suffix-1"),
                targetSpotFifo);
            assertEquals(0, callerFailures.get());
            assertNull(targetFailure.get());
        }
    }

    @Test
    void postCutActorRedirectReachesTheRelocationForwardExactlyOnce()
        throws Exception {
        String endpoint = "inproc://jvm-postcut-target-" + System.nanoTime();
        RoutingId sourceRid = RoutingId.from("jvm-postcut-source");
        RoutingId targetRid = RoutingId.from("jvm-postcut-target");
        RoutingId callerRid = RoutingId.from("jvm-postcut-caller");
        try (var context = Zlink.createContext();
             var source = new ZLinkJavaRawMeshNode(context, "mesh");
             var target = new ZLinkJavaRawMeshNode(context, "mesh")) {
            source.setRoutingId(sourceRid);
            source.setBind("inproc://jvm-postcut-source-" + System.nanoTime());
            target.setRoutingId(targetRid);
            target.setBind(endpoint);
            source.start();
            target.start();
            acceptExactSource(
                target, sourceRid, source.lifecycleGeneration());
            source.connectPeer(endpoint, targetRid);
            awaitAdmitted(source);

            ZLinkJavaRawSpotNode sourceSpots =
                (ZLinkJavaRawSpotNode) source.spotNode();
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = sourceSpots.createActor("moving-actor", 7, create);
            }
            sourceSpots.rememberActorAuthority(actor, 11, 3);
            var sourceRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor, source.lifecycleGeneration(), 11, 3);
            var targetRoute = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(targetRid, "moving-actor", 7),
                target.lifecycleGeneration(),
                12,
                5);

            List<String> targetFifo = new CopyOnWriteArrayList<>();
            CompletableFuture<Void> received = new CompletableFuture<>();
            AtomicReference<Throwable> targetFailure = new AtomicReference<>();
            target.spotNode().setRelocationStagingIngressHandler(
                new ZLinkInternalSpotNode.RelocationStagingIngressHandler() {
                    @Override
                    public boolean handleSpot(
                        ZLinkInternalMeshNode.PeerAuthorityFence peer,
                        ZLinkServiceM6BWireCodec.SpotMessage header,
                        byte[] metadata,
                        java.util.function.Supplier<byte[]> acceptedRecord,
                        int acceptedRecordSizeHint,
                        List<Message> parts,
                        String contentType,
                        java.util.function.Consumer<List<Message>> reply,
                        java.util.function.Consumer<Throwable> failure) {
                        parts.forEach(Message::close);
                        return true;
                    }

                    @Override
                    public boolean handleActor(
                        ZLinkInternalMeshNode.PeerAuthorityFence peer,
                        ZLinkServiceM6BWireCodec.ActorMessage header,
                        java.util.function.Supplier<byte[]> acceptedRecord,
                        List<Message> parts,
                        String contentType,
                        java.util.function.Consumer<List<Message>> reply,
                        java.util.function.Consumer<Throwable> failure) {
                        try {
                            assertEquals(targetRoute, header.target());
                            targetFifo.add(parts.getLast().toUtf8String());
                            received.complete(null);
                        } catch (Throwable error) {
                            targetFailure.compareAndSet(null, error);
                            received.completeExceptionally(error);
                        } finally {
                            parts.forEach(Message::close);
                        }
                        return true;
                    }
                });

            //  The dispatch handler stands in for the Spot runtime: it grabs
            //  the post-cut re-route hook the ingress installed, and the
            //  forward only appears after ingress already passed its lookup -
            //  exactly the cut-versus-enqueue interleaving under test.
            AtomicReference<Boolean> redirected = new AtomicReference<>();
            AtomicInteger redirectCalls = new AtomicInteger();
            ZLinkBackendSpot sourceSpot = sourceSpots.entrySpot();
            sourceSpot.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE
                    || info.actorMessages().isEmpty()) {
                    return;
                }
                var header = info.actorMessages().get(0);
                var payload = info.actorMessages().get(1);
                var redirect = header.relocationRedirect();
                assertNotNull(redirect,
                    "the mesh ingress must install a post-cut re-route hook");
                installActorFollow(
                    source.spotNode(), sourceRoute, targetRoute);
                redirectCalls.incrementAndGet();
                redirected.set(redirect.redirect(
                    header.message(), payload.message()));
                info.actorMessages().forEach(
                    systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorReceived::close);
            });

            var wireHeader = new ZLinkServiceM6BWireCodec.ActorMessage(
                false, 0, null, null, sourceRoute);
            List<Message> parts = List.of(
                Message.from(ZLinkStreamHeaderCodec.encode(
                    new ZLinkStreamHeader(
                        "RelocatedPacket", Map.of(), Optional.empty()))),
                Message.from("post-cut-suffix"));
            AtomicInteger callerFailures = new AtomicInteger();
            boolean accepted = sourceSpots.enqueueRemoteActor(
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                    callerRid, 9, "caller-owner", 7),
                wireHeader,
                () -> new byte[] {1},
                parts,
                null,
                null,
                ignored -> callerFailures.incrementAndGet());
            assertTrue(accepted);

            received.get(3, TimeUnit.SECONDS);
            assertNull(targetFailure.get());
            assertEquals(1, redirectCalls.get());
            assertTrue(redirected.get(),
                "an installed forward must accept the post-cut arrival");
            assertEquals(List.of("post-cut-suffix"), targetFifo);
            Thread.sleep(200);
            assertEquals(0, callerFailures.get(),
                "a redirected arrival must not also raise a stale terminal");
        }
    }

    @Test
    void capacityRejectedActorIngressReportsOneTerminalWithoutSilentDrop()
        throws Exception {
        assertActorDispatchFailureTerminal(new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED,
            "application queue is full"));
    }

    @Test
    void closedActorIngressReportsOneTerminalWithoutSilentDrop()
        throws Exception {
        assertActorDispatchFailureTerminal(
            new IllegalStateException("target Spot is closed"));
    }

    private static void assertActorDispatchFailureTerminal(
        RuntimeException dispatchFailure) throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from(
                "jvm-actor-dispatch-failure-" + System.nanoTime());
            RoutingId callerRid = RoutingId.from(
                "jvm-actor-dispatch-caller-" + System.nanoTime());
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-actor-dispatch-failure-"
                + System.nanoTime());
            node.start();

            ZLinkJavaRawSpotNode spots =
                (ZLinkJavaRawSpotNode) node.spotNode();
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = spots.createActor("dispatch-failure-actor", create);
            }
            spots.rememberActorAuthority(actor, 11, 3);
            spots.entrySpot().onDispatchEvent(
                new ZLinkInternalAsyncSpotDispatchHandler() {
                    @Override
                    public CompletionStage<Void> handleAsync(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendSpotDispatchInfo info) {
                        info.actorMessages().forEach(
                            ZLinkBackendActorReceived::close);
                        return CompletableFuture.failedFuture(dispatchFailure);
                    }
                });

            var route = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor, node.lifecycleGeneration(), 11, 3);
            var header = new ZLinkServiceM6BWireCodec.ActorMessage(
                true, 0, 91L, null, route);
            AtomicInteger failures = new AtomicInteger();
            AtomicInteger replies = new AtomicInteger();
            CompletableFuture<Throwable> terminal = new CompletableFuture<>();
            List<Message> remoteParts = List.of(
                Message.from("header"), Message.from("payload"));
            assertTrue(spots.enqueueRemoteActor(
                new ZLinkInternalMeshNode.PeerAuthorityFence(
                    callerRid, 7, "caller-owner", 5),
                header,
                () -> new byte[] {1},
                remoteParts,
                null,
                replyParts -> {
                    replies.incrementAndGet();
                    replyParts.forEach(Message::close);
                },
                failure -> {
                    failures.incrementAndGet();
                    terminal.complete(failure);
                }));

            assertEquals(dispatchFailure,
                terminal.get(1, TimeUnit.SECONDS));
            Thread.sleep(50);
            assertEquals(1, failures.get());
            assertEquals(0, replies.get());

            try (Message lateReply = Message.from("late")) {
                spots.replyActorNoBind(
                    actor, callerRid, null, 1, 1, List.of(lateReply));
            }
            assertEquals(0, replies.get(),
                "a failed request must remove its pending reply route");

            AtomicInteger localTerminals = new AtomicInteger();
            CompletionStage<Void> local;
            try (Message payload = Message.from("local-send")) {
                local = spots.sendToActorAsync(actor, List.of(payload));
            }
            ExecutionException localFailure = assertThrows(
                ExecutionException.class,
                () -> local.whenComplete((ignored, failure) ->
                        localTerminals.incrementAndGet())
                    .toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertEquals(dispatchFailure, localFailure.getCause());
            assertEquals(1, localTerminals.get());

            CompletionStage<List<Message>> localRequest;
            try (Message payload = Message.from("local-request")) {
                localRequest = spots.requestToActor(
                    actor,
                    List.of(payload),
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(1));
            }
            ExecutionException requestFailure = assertThrows(
                ExecutionException.class,
                () -> localRequest.toCompletableFuture()
                    .get(1, TimeUnit.SECONDS));
            assertEquals(dispatchFailure, requestFailure.getCause());
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
            ZLinkBackendActorRef
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
                    1),
                new ZLinkServiceM6BWireCodec.BoundSessionTail(
                    RoutingId.from("relay-session"),
                    3,
                    7));
            AtomicBoolean callbackCalled = new AtomicBoolean();
            AtomicBoolean failureCalled = new AtomicBoolean();
            spots.setMessageFollowRelayHandler(
                (sourceNodeRid,
                    sourceNodeGeneration,
                    header,
                    acceptedJournalRecord,
                    parts,
                      contentType,

                      reply,
                      failure,
                      terminalRelease) -> {
                    assertEquals(sourceRid, sourceNodeRid);
                    assertEquals(9, sourceNodeGeneration);
                    assertEquals(stale, header);
                    assertEquals("application/json", contentType);
                    assertEquals(2, parts.size());
                      callbackCalled.set(true);
                      parts.forEach(Message::close);
                      terminalRelease.run();
                      return true;
                });

            List<Message> parts = List.of(
                Message.from(ZLinkStreamHeaderCodec.encode(
                    new ZLinkStreamHeader(
                        "RelayReq",
                        Map.of(),
                        Optional.empty()))),
                Message.from("payload"));
            boolean accepted = spots.enqueueRemoteActor(
                sourceRid,
                9,
                stale,
                new byte[0],
                parts,
                "application/json",
                null,
                ignored -> failureCalled.set(true));

            assertTrue(accepted);
            assertTrue(callbackCalled.get());
            assertFalse(failureCalled.get());
            assertEquals("relay-actor", current.actorId());
        }
    }

    @Test
    void retainedMessageFollowFenceMismatchCannotFallBackToTheRetiredActor() {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-follow-retired");
            RoutingId callerRid = RoutingId.from("jvm-m6b-follow-caller");
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-m6b-follow-retired-"
                + System.nanoTime());
            node.start();

            ZLinkJavaRawSpotNode spots =
                (ZLinkJavaRawSpotNode) node.spotNode();
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = spots.createActor("follow-retired-actor", create);
            }
            spots.rememberActorAuthority(actor, 11, 3);

            AtomicInteger relayAttempts = new AtomicInteger();
            AtomicInteger localDispatches = new AtomicInteger();
            spots.setMessageFollowRelayHandler((
                sourceNodeRid,
                sourceNodeGeneration,
                header,
                acceptedJournalRecord,
                parts,
                  contentType,

                  reply,
                  failure,
                  terminalRelease) -> {
                relayAttempts.incrementAndGet();
                return false;
            });
            spots.entrySpot().onDispatchEvent(info -> {
                if (info.event()
                    == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    localDispatches.incrementAndGet();
                    info.actorMessages().forEach(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorReceived::close);
                }
            });

            var wrongLease = new ZLinkServiceM6BWireCodec.ActorMessage(
                false,
                0,
                null,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    node.lifecycleGeneration(),
                    11,
                    4));
            try (Message packet = Message.from(ZLinkStreamHeaderCodec.encode(
                     new ZLinkStreamHeader(
                         "FollowedPacket", Map.of(), Optional.empty())));
                 Message payload = Message.from("stale-payload")) {
                assertFalse(spots.enqueueRemoteActor(
                    callerRid,
                    7,
                    wrongLease,
                    new byte[0],
                    List.of(packet, payload),
                    "application/json",
                    null,
                    ignored -> { }));
            }

            assertEquals(1, relayAttempts.get());
            assertEquals(0, localDispatches.get());

            var currentLease = new ZLinkServiceM6BWireCodec.ActorMessage(
                false,
                0,
                null,
                null,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    actor,
                    node.lifecycleGeneration(),
                    11,
                    3));
            try (Message packet = Message.from(ZLinkStreamHeaderCodec.encode(
                     new ZLinkStreamHeader(
                         "CurrentPacket", Map.of(), Optional.empty())));
                 Message payload = Message.from("current-payload")) {
                assertTrue(spots.enqueueRemoteActor(
                    callerRid,
                    7,
                    currentLease,
                    new byte[0],
                    List.of(packet, payload),
                    "application/json",
                    null,
                    ignored -> { }));
            }
            assertEquals(2, relayAttempts.get());
            assertEquals(1, localDispatches.get());
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
            CompletableFuture<ZLinkBackendReceived> completion;
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
                completion = source.requestToSpot(
                        nodeRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(request),
                        Duration.ofSeconds(1))
                    .toCompletableFuture();
            }

            try (ZLinkBackendReceived received =
                     completion.get(1, TimeUnit.SECONDS)) {
                callbackCount.incrementAndGet();
                assertEquals(
                    "first",
                    received.parts().getFirst().toUtf8String());
            }
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

            ZLinkBackendActorRef actor;
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
            CompletableFuture<ZLinkBackendReceived> result;

            try (Message request = Message.from("request")) {
                result = source.requestToSpot(
                        nodeRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(request),
                        Duration.ofMillis(20))
                    .toCompletableFuture();
            }

            ExecutionException failure = assertThrows(
                ExecutionException.class,
                () -> result.get(1, TimeUnit.SECONDS));
            assertEquals(
                RequestResult.TIMED_OUT,
                ((ZlinkRequestException) failure.getCause()).getResult());
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
            context.options().autoHwmEnabled(false);
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
            awaitAdmitted(right);
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
            AtomicReference<ZLinkBackendReceived> retainedRoute =
                new AtomicReference<>();
            AtomicInteger handlerCalls = new AtomicInteger();
            target.onDispatchEvent(info -> {
                if (info.event() != ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
                    return;
                }
                ZLinkBackendReceived received =
                    target.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
                handlerCalls.incrementAndGet();
                String value = received.parts().getLast().toUtf8String();
                if (received.requestSeq().isPresent()) {
                    try (received) {
                        try (Message reply = Message.from("remote-reply")) {
                            received.reply(List.of(reply));
                        }
                    }
                } else {
                    retainedRoute.set(received);
                    sent.complete(value);
                }
            });

            try (Message message = Message.from("remote-send")) {
                source.sendToSpot(
                        leftRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(message))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }
            assertEquals("remote-send", sent.get(2, TimeUnit.SECONDS));
            awaitOutstandingApplicationLease(context, 0L);
            retainedRoute.getAndSet(null).close();
            awaitOutstandingApplicationLease(context, 0L);

            CompletableFuture<ZLinkBackendReceived> reply;
            try (Message message = Message.from("remote-request")) {
                reply = source.requestToSpot(
                        leftRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(message),
                        Duration.ofSeconds(2))
                    .toCompletableFuture();
            }
            try (ZLinkBackendReceived received =
                     reply.get(2, TimeUnit.SECONDS)) {
                assertEquals(
                    "remote-reply",
                    received.parts().getFirst().toUtf8String());
            }
            assertEquals(2, handlerCalls.get());

            long staleGeneration = target.lifecycleGeneration() + 1;
            source.rememberSpotAuthority(
                leftRid,
                targetRid.toString(),
                staleGeneration,
                77,
                1);
            try (Message message = Message.from("remote-stale-address")) {
                reply = source.requestToSpot(
                        leftRid,
                        targetRid.toString(),
                        staleGeneration,
                        List.of(message),
                        Duration.ofSeconds(2))
                    .toCompletableFuture();
            }
            try (ZLinkBackendReceived received =
                     reply.get(2, TimeUnit.SECONDS)) {
                assertEquals(
                    "remote-reply",
                    received.parts().getFirst().toUtf8String());
            }
            assertEquals(3, handlerCalls.get());

            left.setPeerAuthorityResolver(
                (meshName, candidateRid, candidateGeneration) ->
                    CompletableFuture.completedFuture(
                        candidateRid.equals(leftRid)
                            ? Optional.of(
                                new ZLinkInternalMeshNode
                                    .PeerAuthorityFence(
                                        leftRid,
                                        candidateGeneration,
                                        "target-owner",
                                        1))
                            : Optional.empty()));
            CompletableFuture<ZLinkBackendReceived> stale;
            try (Message message = Message.from("stale-source")) {
                stale = source.requestToSpot(
                        leftRid,
                        targetRid.toString(),
                        target.lifecycleGeneration(),
                        List.of(message),
                        Duration.ofSeconds(2))
                    .toCompletableFuture();
            }
            try (ZLinkBackendReceived received =
                     stale.get(2, TimeUnit.SECONDS)) {
                assertEquals(
                    ZLinkBackendRequestResult.CONFLICT,
                    received.result());
            }
            assertEquals(3, handlerCalls.get());
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
            ZLinkBackendActorRef actor;
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
            try (Message staleAddress = Message.from("stale-address")) {
                assertTrue(node.spotNode().sendToActor(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            nodeRid,
                            actor.actorId(),
                            actor.generation() + 1),
                    List.of(staleAddress),
                    SendFlags.DONT_WAIT));
            }
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
            ZLinkBackendActorRef
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
            context.options().autoHwmEnabled(false);
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
            CompletableFuture<List<systems.zlink.framework.runtime.internal
                .backend.ZLinkBackendActorReceived>> firstDispatch =
                new CompletableFuture<>();
            entry.onDispatchEvent(info -> {
                if (info.event()
                    != ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
                    return;
                }
                List<systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorReceived> received =
                    info.actorMessages();
                if (firstDispatch.complete(received)) {
                    return;
                }
                var first = received.getFirst();
                try (Message reply = Message.from("remote-actor-reply")) {
                    left.spotNode().replyActorNoBind(
                        first.actor(),
                        first.sourceNodeRid(),
                        first.sourceSessionRid(),
                        first.requestId(),
                        first.flags(),
                        List.of(reply));
                } finally {
                    received.forEach(
                        systems.zlink.framework.runtime.internal.backend
                            .ZLinkBackendActorReceived::close);
                }
            });
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = left.spotNode().createActor("actor-remote", create);
            }
            left.spotNode().rememberActorAuthority(actor, 89, 1);
            right.spotNode().rememberActorAuthority(
                actor, 89, 1);

            awaitAdmitted(right);

            CompletionStage<List<Message>> firstRequest;
            try (Message request = Message.from("remote-actor-request")) {
                firstRequest = right.spotNode().requestToActor(
                    actor,
                    List.of(request),
                    SendFlags.DONT_WAIT,
                    Duration.ofSeconds(2));
            }
            List<systems.zlink.framework.runtime.internal.backend
                .ZLinkBackendActorReceived> retained =
                firstDispatch.get(2, TimeUnit.SECONDS);
            assertEquals(1, retained.size());
            var first = retained.getFirst();
            awaitOutstandingApplicationLease(context, 0L);
            first.close();
            awaitOutstandingApplicationLease(context, 0L);
            try (Message response = Message.from("remote-actor-reply")) {
                left.spotNode().replyActorNoBind(
                    first.actor(),
                    first.sourceNodeRid(),
                    first.sourceSessionRid(),
                    first.requestId(),
                    first.flags(),
                    List.of(response));
            }
            List<Message> reply = firstRequest.toCompletableFuture()
                .get(2, TimeUnit.SECONDS);
            try {
                assertEquals(
                    "remote-actor-reply",
                    reply.getFirst().toUtf8String());
            } finally {
                reply.forEach(Message::close);
            }
            ZLinkBackendActorRef staleAddress = new ZLinkBackendActorRef(
                leftRid, actor.actorId(), actor.generation() + 1);
            right.spotNode().rememberActorAuthority(staleAddress, 89, 1);
            try (Message request = Message.from("remote-actor-progress")) {
                reply = right.spotNode().requestToActor(
                    staleAddress,
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
                ZLinkMeshNodeObjectRole.SERVER);
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

            awaitAdmitted(right, leftRid);
            assertEquals(leftRid, awaitPlacementTarget(right));

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

            ZLinkBackendTopicMessage
                received = null;
            long receiveDeadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (received == null
                && System.nanoTime() < receiveDeadline) {
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
            Optional<RoutingId> placement;
            do {
                placement = right.selectPlacementTarget();
                if (placement.isPresent()) {
                    Thread.sleep(1);
                }
            } while (placement.isPresent()
                && System.nanoTime() < placementUpdateDeadline);
            assertTrue(placement.isEmpty());
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

            awaitAdmitted(right, leftRid);

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
                right.sendInstanceSpot(
                        route,
                        "orders",
                        null,
                        new byte[] {3},
                        List.of(packet, payload))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }

            ZLinkBackendSpot activated = null;
            long activationDeadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();
            while (activated == null
                && System.nanoTime() < activationDeadline) {
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
                route.objectGeneration() - 1,
                route.ownerId(),
                route.authorityOwnerGeneration() - 1,
                route.leaseGeneration() - 1,
                "store-2");
            try (Message packet = Message.from("Packet");
                 Message payload = Message.from("stale")) {
                right.sendInstanceSpot(
                        stale, "orders", null, new byte[0],
                        List.of(packet, payload))
                    .toCompletableFuture()
                    .get(2, TimeUnit.SECONDS);
            }
            var staleDeadline = System.nanoTime()
                + Duration.ofSeconds(2).toNanos();
            var reused = activated.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
            while (reused == null && System.nanoTime() < staleDeadline) {
                Thread.sleep(1);
                reused = activated.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
            }
            assertNotNull(reused);
            try (var reusedDelivery = reused) {
                assertEquals(
                    "stale",
                    reusedDelivery.parts().getLast().toUtf8String());
            }
            assertEquals(41, activated.lifecycleGeneration());
        }
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
            ZLinkBackendActorRef actor;
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
    void remoteUnbindIsIdempotentAfterActorIsNoLongerCurrent()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-unbind-node");
            RoutingId sourceRid = RoutingId.from("jvm-m6b-unbind-source");
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-m6b-unbind-" + System.nanoTime());
            node.start();

            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) node.spotNode();
            var actor = new systems.zlink.framework.runtime.internal.backend
                .ZLinkBackendActorRef(nodeRid, "retired-actor", 1);
            var route = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor,
                node.lifecycleGeneration(),
                41,
                1);
            var inactive = new ZLinkServiceM6BWireCodec.BoundSessionBind(
                1,
                route,
                RoutingId.from("jvm-m6b-unbind-session"),
                false,
                3);

            assertTrue(target.acceptRemoteStreamBinding(
                sourceRid, 7, inactive));
            assertTrue(target.acceptRemoteStreamBinding(
                sourceRid, 7, inactive));
            assertFalse(target.acceptRemoteStreamBinding(
                sourceRid,
                7,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    2,
                    route,
                    inactive.sessionRid(),
                    true,
                    3)));
        }
    }

    @Test
    void command44TargetBindAcceptsPreinstalledFenceIdempotently()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            RoutingId nodeRid = RoutingId.from("jvm-m6b-route-target");
            RoutingId sessionOwnerRid =
                RoutingId.from("jvm-m6b-route-session-owner");
            RoutingId sessionRid = RoutingId.from("jvm-m6b-route-session");
            node.setRoutingId(nodeRid);
            node.setBind("inproc://jvm-m6b-route-" + System.nanoTime());
            node.start();

            ZLinkJavaRawSpotNode target =
                (ZLinkJavaRawSpotNode) node.spotNode();
            ZLinkBackendActorRef actor;
            try (Message create = Message.from("create")) {
                actor = target.createActor("route-actor", create);
            }
            target.rememberActorAuthority(actor, 41, 7);
            var route = new ZLinkServiceM6BWireCodec.ActorRouteFence(
                actor,
                node.lifecycleGeneration(),
                41,
                7);
            var session = new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                sessionOwnerRid,
                11,
                sessionOwnerRid.toString(),
                1,
                sessionRid,
                5);
            target.installRelocatingActorBoundSession(route, session);

            assertTrue(target.acceptRemoteStreamBinding(
                sessionOwnerRid,
                11,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    1, route, sessionRid, true, 5)));
            assertEquals(
                5,
                target.boundSessionRoute(actor).orElseThrow()
                    .bindingGeneration());

            assertTrue(target.acceptRemoteStreamBinding(
                sessionOwnerRid,
                11,
                new ZLinkServiceM6BWireCodec.BoundSessionBind(
                    2, route, sessionRid, true, 6)));
            assertEquals(
                6,
                target.boundSessionRoute(actor).orElseThrow()
                    .bindingGeneration());
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
            ZLinkBackendActorRef
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
    void streamCloseCompletesWhenRemoteBindingRouteIsAlreadyGone()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from("disconnected-close-node"));
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
                        return CompletableFuture.failedFuture(
                            new ZlinkRequestException(
                                RequestResult.NOT_CONNECTED));
                    }
                };
            var stream = new ZLinkJavaStreamSocket(
                context.createStreamSocket(),
                node,
                null,
                lifecycle);
            stream.startSessionService();
            stream.bindActor(
                    RoutingId.from("disconnected-close-session"),
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            RoutingId.from("disconnected-close-actor-node"),
                            "disconnected-close-actor",
                            1))
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            assertDoesNotThrow(stream::close);
        }
    }

    @Test
    void streamCloseCompletesWhenRemoteBindingAdmissionIsGone()
        throws Exception {
        try (var context = Zlink.createContext();
             var node = new ZLinkJavaRawMeshNode(context, "mesh")) {
            node.setRoutingId(RoutingId.from("not-admitted-close-node"));
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
                        return CompletableFuture.failedFuture(
                            new ZlinkSubmitException(
                                SubmitResult.NOT_ADMITTED));
                    }
                };
            var stream = new ZLinkJavaStreamSocket(
                context.createStreamSocket(),
                node,
                null,
                lifecycle);
            stream.startSessionService();
            stream.bindActor(
                    RoutingId.from("not-admitted-close-session"),
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            RoutingId.from("not-admitted-close-actor-node"),
                            "not-admitted-close-actor",
                            1))
                .submit(Duration.ofSeconds(1)).toCompletableFuture()
                .get(1, TimeUnit.SECONDS);

            assertDoesNotThrow(stream::close);
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
            ZLinkBackendActorRef
                actor;
            try (Message create = Message.from("create")) {
                actor = actorNode.spotNode().createActor(
                    "actor-bound-remote", create);
            }
            actorNode.spotNode().rememberActorAuthority(actor, 73, 1);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            awaitAdmitted(sessionNode, actorNodeRid);
            // The first bound push below travels in the reverse direction.
            // Admission is directional, so the session->actor readiness used
            // for bind does not prove actor->session submission is ready.
            awaitAdmitted(actorNode, sessionNodeRid);
            long deadline =
                System.nanoTime() + Duration.ofSeconds(2).toNanos();

            stream.startSessionService();
            ZLinkBackendStreamSocket ownerStream =
                (ZLinkBackendStreamSocket) Proxy.newProxyInstance(
                    ZLinkBackendStreamSocket.class.getClassLoader(),
                    new Class<?>[] {ZLinkBackendStreamSocket.class},
                    (proxy, method, arguments) -> {
                        if (method.getName().equals("requestBoundActor")) {
                            return CompletableFuture.completedFuture(List.of());
                        }
                        try {
                            return method.invoke(stream, arguments);
                        } catch (InvocationTargetException failure) {
                            throw failure.getCause();
                        }
                    });
            var sessionOwner = new ZLinkSessionActorsRuntime(
                sessionNode.spotNode(),
                ownerStream,
                sessionRid,
                null,
                new ZLinkStringMessageSerializer(),
                ignored -> true,
                null,
                true,
                ZLinkStreamCodec.RAW);
            sessionNode.setBoundSessionSendHandler((
                sourceNodeRid,
                sourceNodeGeneration,
                command,
                payload) -> sessionOwner.matchesBoundSessionSend(
                    sourceNodeRid, sourceNodeGeneration, command)
                    && sessionOwner.acceptBoundSessionSend(
                        sourceNodeRid,
                        sourceNodeGeneration,
                        command,
                        payload));
            long firstBindingGeneration =
                sessionNode.bindingGenerationSeed();
            sessionOwner.bind(new ActorRef(
                    actor.actorId(), actor.generation(), "mesh", actor.nodeRid()))
                .toCompletableFuture()
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
                actorNode.spotNode().sendRemoteActorBoundSession(
                        actor, List.of(push))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
            }
            while (pushed.isEmpty()
                && System.nanoTime() < deadline) {
                Thread.sleep(1);
            }
            assertEquals(List.of("push-one"), pushed);
            sessionNode.spotNode().rememberActorAuthority(actor, 73, 1);

            sessionOwner.bind(new ActorRef(
                    actor.actorId(), actor.generation(), "mesh", actor.nodeRid()))
                .toCompletableFuture()
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
                actorNode.spotNode().sendRemoteActorBoundSession(
                        actor, List.of(push))
                    .toCompletableFuture()
                    .get(1, TimeUnit.SECONDS);
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
                ExecutionException lateFailure = assertThrows(
                    ExecutionException.class,
                    () -> actorNode.spotNode()
                        .sendRemoteActorBoundSession(actor, List.of(late))
                        .toCompletableFuture()
                        .get(1, TimeUnit.SECONDS));
                assertEquals(
                    SubmitResult.NOT_FOUND,
                    ((ZlinkSubmitException) lateFailure.getCause()).getResult());
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
                        ? Optional.of(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                candidateRid,
                                candidateGeneration,
                                "test-source-owner",
                                1))
                        : Optional.empty()));
    }

    private static void installActorFollow(
        ZLinkInternalSpotNode source,
        ZLinkServiceM6BWireCodec.ActorRouteFence sourceRoute,
        ZLinkServiceM6BWireCodec.ActorRouteFence targetRoute) {
        source.setMessageFollowRelayHandler((
            sourceNodeRid,
            sourceNodeGeneration,
            header,
            acceptedJournalRecord,
            parts,
            contentType,
            reply,
            failure,
            terminalRelease) -> {
                if (!sourceRoute.equals(header.target())) {
                    return false;
                }
                source.forwardMessageFollowActor(header, targetRoute, parts)
                    .whenComplete((replies, relayFailure) -> {
                        try {
                            if (relayFailure != null) {
                                failure.accept(relayFailure);
                            } else if (!replies.isEmpty()) {
                                reply.accept(replies);
                            }
                        } finally {
                            terminalRelease.run();
                        }
                    });
                return true;
            });
    }

    private static void awaitOutstandingApplicationLease(
        systems.zlink.contracts.core.Context context,
        long expected) throws InterruptedException {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        long outstanding = context.coreHwmBudgetSnapshot()
            .outstandingApplicationLeaseCount();
        while (outstanding != expected && System.nanoTime() < deadline) {
            Thread.sleep(1);
            outstanding = context.coreHwmBudgetSnapshot()
                .outstandingApplicationLeaseCount();
        }
        assertEquals(expected, outstanding);
    }
}
