package systems.zlink.framework.runtime.host;
import java.nio.charset.StandardCharsets;
import java.lang.reflect.Proxy;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntimeTestAccess;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinEntrySpotResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinRequest;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorLifecycleEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendDealerSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendPublisherSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRecvMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRouterSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchEvent;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotNodeMode;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotRouteBridge;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSubscriberSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceFrozenRecordCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.spots.ZLinkSpotKind;

final class EntrySpotActorDispatchTests {
    private static final int NO_BIND = 1;
    private static final ZLinkJsonMessageSerializer SERIALIZER = new ZLinkJsonMessageSerializer();
    private static final ZLinkMessageSerializer PROTOBUF_SERIALIZER =
        new ProbeProtobufSerializer();

    @Test
    void entrySpotActorDispatchNoBindRequestRepliesViaNoBindAndDoesNotBindSession() throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts(
                "actor-a",
                "request",
                "ok",
                42,
                NO_BIND,
                EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
                Map.of("trace-id", "trace-1")));

            ReplyRecord reply = awaitSingle(backend.node.noBindReplies);
            assertEquals("actor-a", reply.actor().actorId());
            assertEquals(RoutingId.from("source-node"), reply.sourceNodeRid());
            assertEquals(RoutingId.from("source-session"), reply.sourceSessionRid());
            assertEquals(42, reply.requestId());
            assertEquals(NO_BIND, reply.flags());
            assertTrue(backend.node.boundSessionReplies.isEmpty());
            assertEquals(0, backend.node.remoteSessionBinds.size());

            DecodedFrame frame = decodeFrame(reply.parts().get(0));
            assertEquals(ZLinkStreamMessageKind.RESPONSE, frame.header().kind());
            assertEquals("", frame.header().packetName());
            assertEquals(ZLinkStreamCodec.JSON, frame.header().codec());
            assertEquals(Map.of(), frame.header().metadata());
            assertFalse(frame.header().flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
            assertEquals("ok:actor-a", deserializeReply(frame).value());
        }
    }

    @Test
    void consecutiveProtobufActorRequestsEachProduceOneDecodableReply()
        throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend, true)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts(
                "actor-a", "request", "first", 51, NO_BIND,
                ZLinkStreamCodec.PROTOBUF, PROTOBUF_SERIALIZER));
            ReplyRecord first = awaitReplyCount(backend.node.noBindReplies, 1)
                .getFirst();
            DecodedFrame firstFrame = decodeFrame(first.parts().getFirst());
            assertEquals(ZLinkStreamMessageKind.RESPONSE, firstFrame.header().kind());
            assertEquals(ZLinkStreamCodec.PROTOBUF, firstFrame.header().codec());
            assertEquals(
                new ProbeReply("first:actor-a:application/x-protobuf"),
                PROTOBUF_SERIALIZER.deserialize(
                    ZLinkEncodedPayload.from(firstFrame.body()), ProbeReply.class));

            backend.entrySpot.raiseActorReadable(actorRequestParts(
                "actor-a", "request", "second", 52, NO_BIND,
                ZLinkStreamCodec.PROTOBUF, PROTOBUF_SERIALIZER));
            List<ReplyRecord> replies =
                awaitReplyCount(backend.node.noBindReplies, 2);
            DecodedFrame secondFrame = decodeFrame(
                replies.get(1).parts().getFirst());
            assertEquals(ZLinkStreamMessageKind.RESPONSE, secondFrame.header().kind());
            assertEquals(ZLinkStreamCodec.PROTOBUF, secondFrame.header().codec());
            assertEquals(
                new ProbeReply("second:actor-a:application/x-protobuf"),
                PROTOBUF_SERIALIZER.deserialize(
                    ZLinkEncodedPayload.from(secondFrame.body()), ProbeReply.class));
            Thread.sleep(25);
            assertEquals(2, backend.node.noBindReplies.size());
        }
    }

    @Test
    void messageFollowRelayKeepsTheOriginalOperationExactlyOnceAtTheTarget()
        throws Exception {
        TestBackend backend = startBackend();
        ZLinkActorRuntime sourceRuntime = messageFollowSourceRuntime();
        try (ZLinkFrameworkRuntime targetRuntime = startRuntime(backend)) {
            ZLinkActorRuntime targetActors =
                (ZLinkActorRuntime) targetRuntime.actorManager();
            targetRuntime.actorManager().create("follow-send-actor", "probe")
                .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);
            targetRuntime.actorManager().create("follow-request-actor", "probe")
                .submit().toCompletableFuture().get(5, TimeUnit.SECONDS);

            ProbeActor targetSendActor = (ProbeActor) targetActors
                .localActor("follow-send-actor").orElseThrow();
            ProbeActor targetRequestActor = (ProbeActor) targetActors
                .localActor("follow-request-actor").orElseThrow();
            ZLinkActor sourceSendActor = sourceRuntime
                .getOrCreateLocalActor("follow-send-actor", ProbeActor.class)
                .toCompletableFuture().get(5, TimeUnit.SECONDS)
                .orElseThrow();
            ZLinkActor sourceRequestActor = sourceRuntime
                .getOrCreateLocalActor("follow-request-actor", ProbeActor.class)
                .toCompletableFuture().get(5, TimeUnit.SECONDS)
                .orElseThrow();

            sourceRuntime.setRoutedTransport(
                (ZLinkChannelRuntime) targetRuntime.route(),
                () -> "message-follow-source-spot");
            retainMessageFollow(
                sourceRuntime,
                sourceSendActor,
                targetActors.currentRef(targetSendActor),
                backend.entrySpot.spotId());
            retainMessageFollow(
                sourceRuntime,
                sourceRequestActor,
                targetActors.currentRef(targetRequestActor),
                backend.entrySpot.spotId());
            sourceRuntime.setMessageFollowNoticeSender(
                (target, notice) -> CompletableFuture.completedFuture(null));

            AtomicInteger generationFailures = new AtomicInteger();
            AtomicReference<Throwable> generationFailure =
                new AtomicReference<>();
            assertTrue(relayMessageFollow(
                sourceRuntime,
                new ZLinkBackendActorRef(
                    sourceRuntime.currentRef(sourceSendActor).nodeRid(),
                    "follow-send-actor",
                    sourceRuntime.currentRef(sourceSendActor).generation() + 1),
                false,
                0,
                100,
                200,
                "follow-send",
                SERIALIZER.serialize(new ProbeRequest("generation")).bytes(),
                new byte[] {1},
                new AtomicInteger(),
                generationFailures,
                generationFailure));
            assertEquals(1, generationFailures.get());
            assertEquals(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                assertInstanceOf(
                    ZLinkFrameworkException.class,
                    generationFailure.get()).kind());

            byte[] sendPayload = SERIALIZER.serialize(
                new ProbeRequest("follow-send")).bytes();
            byte[] sendRecord = acceptedActorRecord(
                "follow-send-actor",
                false,
                0,
                101,
                201,
                "follow-send",
                sendPayload);
            AtomicInteger sendFailures = new AtomicInteger();
            AtomicReference<Throwable> sendFailure = new AtomicReference<>();
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceSendActor),
                false,
                0,
                101,
                201,
                "follow-send",
                sendPayload,
                sendRecord,
                new AtomicInteger(),
                sendFailures,
                sendFailure));
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceSendActor),
                false,
                0,
                101,
                201,
                "follow-send",
                sendPayload,
                sendRecord,
                new AtomicInteger(),
                sendFailures,
                sendFailure));
            byte[] requestPayload = SERIALIZER.serialize(
                new ProbeRequest("follow-request")).bytes();
            byte[] requestRecord = acceptedActorRecord(
                "follow-request-actor",
                true,
                77,
                102,
                202,
                "request",
                requestPayload);
            AtomicInteger requestReplies = new AtomicInteger();
            AtomicInteger requestFailures = new AtomicInteger();
            AtomicReference<Throwable> requestFailure = new AtomicReference<>();
            AtomicInteger staleFenceReplies = new AtomicInteger();
            AtomicInteger staleFenceFailures = new AtomicInteger();
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceRequestActor),
                true,
                77,
                102,
                202,
                4,
                "request",
                requestPayload,
                requestRecord,
                staleFenceReplies,
                staleFenceFailures,
                new AtomicReference<>()));
            assertEquals(0, staleFenceReplies.get());
            assertEquals(1, staleFenceFailures.get());
            assertEquals(0, targetRequestActor.handled());
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceRequestActor),
                true,
                77,
                102,
                202,
                "request",
                requestPayload,
                requestRecord,
                requestReplies,
                requestFailures,
                requestFailure));
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceRequestActor),
                true,
                77,
                102,
                202,
                "request",
                requestPayload,
                requestRecord,
                requestReplies,
                requestFailures,
                requestFailure));
            awaitHandled(
                targetRequestActor,
                1,
                requestFailure);
            awaitCount(requestReplies, 1);
            assertEquals(0, requestFailures.get());
            awaitHandled(
                targetSendActor,
                1,
                sendFailure);

            sourceRuntime.setMessageFollowNoticeSender(null);
            byte[] noNoticeRecord = acceptedActorRecord(
                "follow-send-actor",
                false,
                0,
                103,
                203,
                "follow-send",
                sendPayload);
            AtomicInteger noNoticeFailures = new AtomicInteger();
            AtomicReference<Throwable> noNoticeFailure =
                new AtomicReference<>();
            assertTrue(relayMessageFollow(
                sourceRuntime,
                sourceRuntime.currentRef(sourceSendActor),
                false,
                0,
                103,
                203,
                "follow-send",
                sendPayload,
                noNoticeRecord,
                new AtomicInteger(),
                noNoticeFailures,
                noNoticeFailure));
            awaitHandled(
                targetSendActor,
                2,
                noNoticeFailure);
            assertEquals(0, noNoticeFailures.get());
        } finally {
            sourceRuntime.close();
        }
    }

    @Test
    void entrySpotActorDispatchBoundRequestUsesBoundSessionAndBindsSession() throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts("actor-a", "request", "bound", 0, 0));

            ReplyRecord reply = awaitSingle(backend.node.boundSessionReplies);
            assertEquals("actor-a", reply.actor().actorId());
            assertTrue(backend.node.noBindReplies.isEmpty());
            assertEquals(1, backend.node.remoteSessionBinds.size());

            DecodedFrame frame = decodeFrame(reply.parts().get(0));
            assertEquals(ZLinkStreamMessageKind.RESPONSE, frame.header().kind());
            assertEquals("", frame.header().packetName());
            assertEquals(ZLinkStreamCodec.JSON, frame.header().codec());
            assertEquals("bound:actor-a", deserializeReply(frame).value());
        }
    }

    @Test
    void boundRequestKeepsTheSessionOwnerBindingGenerationFromTheRuntimePort()
        throws Exception {
        TestBackend backend = startBackend();
        long sessionOwnerBindingGeneration = 4_325_711_503_797_842_283L;
        backend.node.canonicalBoundSessionRoute = Optional.of(
            new ZLinkInternalSpotNode.BoundSessionRoute(
                RoutingId.from("source-node"),
                17,
                RoutingId.from("source-session"),
                sessionOwnerBindingGeneration));
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts(
                "actor-a", "request", "bound", 0, 0));
            awaitSingle(backend.node.boundSessionReplies);

            ZLinkActorRuntime actors = (ZLinkActorRuntime) runtime.actorManager();
            ZLinkActor actor = actors.localActor("actor-a").orElseThrow();
            assertEquals(
                sessionOwnerBindingGeneration,
                actors.boundSessionRoute(actor).orElseThrow()
                    .bindingGeneration(),
                "the Actor relocation fence must retain the generation "
                    + "accepted by the Session owner runtime port");
        }
    }


    @Test
    void entrySpotActorDispatchNoBindHandlerExceptionRepliesNoBindError() throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts("actor-a", "throw", "boom", 43, NO_BIND));

            ReplyRecord reply = awaitSingle(backend.node.noBindReplies);
            DecodedFrame frame = decodeFrame(reply.parts().get(0));
            assertEquals(ZLinkStreamMessageKind.ERROR, frame.header().kind());
            assertTrue(new String(frame.body(), StandardCharsets.UTF_8).contains("boom"));
            assertTrue(backend.node.boundSessionReplies.isEmpty());
            assertEquals(0, backend.node.remoteSessionBinds.size());

        }
    }

    @Test
    void entrySpotActorDispatchNoBindMissingActorRepliesNoBindError() throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            backend.entrySpot.raiseActorReadable(actorRequestParts("missing", "request", "missing", 44, NO_BIND));

            ReplyRecord reply = awaitSingle(backend.node.noBindReplies);
            DecodedFrame frame = decodeFrame(reply.parts().get(0));
            assertEquals(ZLinkStreamMessageKind.ERROR, frame.header().kind());
            assertTrue(new String(frame.body(), StandardCharsets.UTF_8)
                .contains("not registered locally"));
            assertTrue(backend.node.boundSessionReplies.isEmpty());
            assertEquals(0, backend.node.remoteSessionBinds.size());
        }
    }

    @Test
    void postCutActorArrivalIsReRoutedThroughTheRelocationForwardExactlyOnce()
        throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);
            ZLinkActorRuntime actors = (ZLinkActorRuntime) runtime.actorManager();
            ProbeActor actor = (ProbeActor)
                actors.localActor("actor-a").orElseThrow();
            awaitActorCreationTurn(actors);

            //  Take the Actor queue past its relocation cut: from here every
            //  admission is rejected with RelocatedOwnerException, which is the
            //  exact post-cut window the ingress used to swallow.
            var seal = actors.trySealActorRelocation("actor-a").orElseThrow();
            var commit = actors.retainActorRelocationCommit("actor-a", seal)
                .orElseThrow();
            var cut = commit.cut();
            while (!commit.tryEstablishAndFinishCapture(cut)) {
                cut = commit.cut();
            }

            AtomicInteger redirects = new AtomicInteger();
            List<String> forwarded = new CopyOnWriteArrayList<>();
            backend.entrySpot.raiseActorReadable(postCutSendParts(
                "actor-a",
                "follow-send",
                "post-cut",
                (headerFrame, payloadFrame) -> {
                    redirects.incrementAndGet();
                    forwarded.add(payloadFrame.toUtf8String());
                    return true;
                }));

            awaitCount(redirects, 1);
            assertEquals(1, forwarded.size());
            Thread.sleep(100);
            assertEquals(1, redirects.get(),
                "the post-cut arrival must reach the relocation forward once");
            assertEquals(0, actor.handled(),
                "the relocated source must not run the post-cut turn");
        }
    }

    @Test
    void postCutActorArrivalWithoutForwardKeepsTheStaleTerminal()
        throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);
            ZLinkActorRuntime actors = (ZLinkActorRuntime) runtime.actorManager();
            ProbeActor actor = (ProbeActor)
                actors.localActor("actor-a").orElseThrow();
            awaitActorCreationTurn(actors);

            var seal = actors.trySealActorRelocation("actor-a").orElseThrow();
            var commit = actors.retainActorRelocationCommit("actor-a", seal)
                .orElseThrow();
            var cut = commit.cut();
            while (!commit.tryEstablishAndFinishCapture(cut)) {
                cut = commit.cut();
            }

            AtomicInteger redirects = new AtomicInteger();
            backend.entrySpot.raiseActorReadable(postCutSendParts(
                "actor-a",
                "follow-send",
                "post-cut",
                (headerFrame, payloadFrame) -> {
                    redirects.incrementAndGet();
                    return false;
                }));

            awaitCount(redirects, 1);
            Thread.sleep(100);
            assertEquals(1, redirects.get());
            assertEquals(0, actor.handled(),
                "a refused forward must not replay the turn on the source");
        }
    }

    @Test
    void postCutMultipartActorArrivalIsNotRedirectedAndIsNotReplayed()
        throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);
            ZLinkActorRuntime actors = (ZLinkActorRuntime) runtime.actorManager();
            ProbeActor actor = (ProbeActor)
                actors.localActor("actor-a").orElseThrow();
            awaitActorCreationTurn(actors);

            var seal = actors.trySealActorRelocation("actor-a").orElseThrow();
            var commit = actors.retainActorRelocationCommit("actor-a", seal)
                .orElseThrow();
            var cut = commit.cut();
            while (!commit.tryEstablishAndFinishCapture(cut)) {
                cut = commit.cut();
            }

            //  The mesh ingress installs no re-route hook for a shape the
            //  relocation forward cannot re-encode frame for frame, so the
            //  rejection has to stay the terminal instead of being truncated.
            List<ZLinkBackendActorReceived> parts =
                new ArrayList<>(postCutSendParts(
                    "actor-a",
                    "follow-send",
                    "post-cut-multipart",
                    null));
            parts.add(new ZLinkBackendActorReceived(
                new ZLinkBackendActorRef(
                    RoutingId.from("entry-node"), "actor-a", 1),
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                0,
                0,
                Message.from("trailer".getBytes(StandardCharsets.UTF_8)),
                false));
            backend.entrySpot.raiseActorReadable(List.copyOf(parts));

            Thread.sleep(200);
            assertNull(parts.get(0).relocationRedirect(),
                "a non-canonical part shape must carry no re-route hook");
            assertEquals(0, actor.handled(),
                "a relocated source must not replay a post-cut multipart turn");
        }
    }

    private static List<ZLinkBackendActorReceived> postCutSendParts(
        String actorId,
        String packetName,
        String value,
        ZLinkBackendActorReceived.RelocationRedirect redirect) {
        ZLinkBackendActorRef actorRef =
            new ZLinkBackendActorRef(RoutingId.from("entry-node"), actorId, 1);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName,
            Map.of());
        byte[] payload = SERIALIZER.serialize(new ProbeRequest(value)).bytes();
        byte[] accepted = acceptedActorRecord(
            actorId, false, 0, 301, 401, packetName, payload);
        return List.of(
            ZLinkBackendActorReceived.lazyJournal(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                0,
                0,
                Message.from(ZLinkStreamHeaderCodec.encode(header)),
                true,
                () -> accepted,
                "application/zlink-framework-json-v1",
                () -> { },
                redirect),
            new ZLinkBackendActorReceived(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                0,
                0,
                Message.from(payload),
                false));
    }

    private static TestBackend startBackend() {
        TestBackend backend = new TestBackend();
        return backend;
    }

    private static ZLinkFrameworkRuntime startRuntime(TestBackend backend) {
        return startRuntime(backend, false);
    }

    private static ZLinkFrameworkRuntime startRuntime(
        TestBackend backend,
        boolean protobuf) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(1));
        options.addHandlersFromPackageOf(EntrySpotActorDispatchTests.class);
        if (protobuf) {
            options.codecs().use(codecs -> {
                codecs.addSerializer(
                    "application/x-protobuf",
                    PROTOBUF_SERIALIZER,
                    type -> type == ProbeRequest.class
                        || type == ProbeReply.class);
                codecs.addStreamCodec(
                    "application/x-protobuf", ZLinkStreamCodec.PROTOBUF);
            });
        }
        var node = systems.zlink.framework.runtime.internal.configuration
            .ZLinkLegacyTopology.addSpotMesh(options, "entry");
        node.setRoutingId(RoutingId.from("entry-node"));
        node.enableRouter("inproc://entry-actor-dispatch");
        node.objects()
            .server()
            .addEntrySpot(ProbeEntrySpot.class)
            .addActorFactory(
                "probe",
                ProbeActor.class,
                ProbeActorFactory.class,
                factory -> factory.disableRelocation());
        return ZLinkFrameworkRuntimeTestAccess.start(options, backend);
    }

    private static ZLinkActorRuntime messageFollowSourceRuntime() {
        RoutingId sourceRid = RoutingId.from("message-follow-source");
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode)
            Proxy.newProxyInstance(
                ZLinkInternalSpotNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalSpotNode.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "routingId" -> sourceRid;
                    case "createActor" -> {
                        ((Message) arguments[1]).close();
                        yield new ZLinkBackendActorRef(
                            sourceRid, (String) arguments[0], 1);
                    }
                    case "actorNodeGeneration" -> 5L;
                    case "actorAuthorityOwnerGeneration" -> 11L;
                    case "actorAuthorityOwnerLeaseGeneration" -> 3L;
                    case "close" -> null;
                    default -> defaultValue(method.getReturnType());
                });
        return new ZLinkActorRuntime(
            node,
            Map.of("probe", ProbeActorFactory.class),
            Map.of(),
            Duration.ofSeconds(1),
            Duration.ofSeconds(5),
            SERIALIZER,
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.JSON);
    }

    private static void retainMessageFollow(
        ZLinkActorRuntime sourceRuntime,
        ZLinkActor sourceActor,
        ZLinkBackendActorRef targetActorRef,
        String targetSpotId) {
        var address = new SpotTransportAddress(
            "entry",
            targetActorRef.nodeRid(),
            targetSpotId,
            0,
            1,
            11,
            3,
            ZLinkSpotKind.ENTRY);
        var targetRoute = new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            targetActorRef.actorId(),
            targetActorRef.generation(),
            targetActorRef.nodeRid(),
            1,
            11,
            3);
        ZLinkActorRuntimeTestAccess.retainMessageFollowSource(
            sourceRuntime,
            sourceActor,
            sourceRuntime.currentRef(sourceActor),
            targetActorRef,
            address,
            targetRoute);
    }

    private static byte[] acceptedActorRecord(
        String actorId,
        boolean request,
        long requestId,
        long operationHigh,
        long operationLow,
        String packetName,
        byte[] payload) {
        RoutingId sourceRid = RoutingId.from("message-follow-source");
        var owner = new ZLinkInternalMeshNode.PeerAuthorityFence(
            sourceRid, 5, "message-follow-owner", 1);
        var operation = new ZLinkServiceM6BWireCodec.ActorMessage(
            request,
            0,
            request ? requestId : null,
            operationHigh,
            operationLow,
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new ZLinkBackendActorRef(sourceRid, actorId, 1),
                5,
                11,
                3));
        var wire = new ZLinkServiceM6AWireCodec();
        return ZLinkServiceFrozenRecordCodec.encodeActor(
            owner,
            owner,
            operation,
            new byte[0],
            wire.encodeApplicationPayload(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    packetName,
                    "application/zlink-framework-json-v1",
                    payload)));
    }

    private static boolean relayMessageFollow(
        ZLinkActorRuntime sourceRuntime,
        ZLinkBackendActorRef sourceActorRef,
        boolean request,
        long requestId,
        long operationHigh,
        long operationLow,
        String packetName,
        byte[] payload,
        byte[] acceptedJournalRecord,
        AtomicInteger replies,
        AtomicInteger failures,
        AtomicReference<Throwable> lastFailure) {
        return relayMessageFollow(
            sourceRuntime,
            sourceActorRef,
            request,
            requestId,
            operationHigh,
            operationLow,
            3,
            packetName,
            payload,
            acceptedJournalRecord,
            replies,
            failures,
            lastFailure);
    }

    private static boolean relayMessageFollow(
        ZLinkActorRuntime sourceRuntime,
        ZLinkBackendActorRef sourceActorRef,
        boolean request,
        long requestId,
        long operationHigh,
        long operationLow,
        long ownerLeaseGeneration,
        String packetName,
        byte[] payload,
        byte[] acceptedJournalRecord,
        AtomicInteger replies,
        AtomicInteger failures,
        AtomicReference<Throwable> lastFailure) {
        ZLinkStreamHeader streamHeader = new ZLinkStreamHeader(
            request ? ZLinkStreamMessageKind.REQUEST
                : ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            request ? Optional.of(requestId) : Optional.empty(),
            packetName,
            Map.of());
        var wireHeader = new ZLinkServiceM6BWireCodec.ActorMessage(
            request,
            0,
            request ? requestId : null,
            operationHigh,
            operationLow,
            0,
            null,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                sourceActorRef,
                5,
                11,
                ownerLeaseGeneration));
        List<Message> parts = List.of(
            Message.from(ZLinkStreamHeaderCodec.encode(streamHeader)),
            Message.from(payload));
        boolean owned = sourceRuntime.relayMessageFollow(
            RoutingId.from("message-follow-client"),
            7,
            wireHeader,
            acceptedJournalRecord,
            parts,
            "application/zlink-framework-json-v1",
            replyParts -> {
                replies.incrementAndGet();
                replyParts.forEach(Message::close);
            },
              failure -> {
                  failures.incrementAndGet();
                  lastFailure.compareAndSet(null, failure);
              },
              () -> { });
        if (!owned) {
            parts.forEach(Message::close);
        }
        return owned;
    }

    private static void awaitHandled(
        ProbeActor actor,
        int expected,
        AtomicReference<Throwable> failure)
        throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (actor.handled() != expected
            && failure.get() == null
            && System.nanoTime() < deadline) {
            Thread.sleep(10);
        }
        if (failure.get() != null) {
            throw new AssertionError("Message Follow relay failed", failure.get());
        }
        assertEquals(expected, actor.handled());
    }

    private static void awaitCount(AtomicInteger count, int expected)
        throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (count.get() != expected && System.nanoTime() < deadline) {
            Thread.sleep(10);
        }
        assertEquals(expected, count.get());
    }

    private static void awaitActorCreationTurn(ZLinkActorRuntime actors)
        throws Exception {
        actors.awaitDrainBarrier().toCompletableFuture()
            .get(5, TimeUnit.SECONDS);
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) {
            return null;
        }
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    private static List<ZLinkBackendActorReceived> actorRequestParts(
        String actorId,
        String packetName,
        String value,
        long requestId,
        int flags) {
        return actorRequestParts(
            actorId,
            packetName,
            value,
            requestId,
            flags,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Map.of());
    }

    private static List<ZLinkBackendActorReceived> actorRequestParts(
        String actorId,
        String packetName,
        String value,
        long requestId,
        int flags,
        ZLinkStreamCodec codec,
        ZLinkMessageSerializer serializer) {
        ZLinkBackendActorRef actorRef =
            new ZLinkBackendActorRef(RoutingId.from("entry-node"), actorId, 1);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            codec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(requestId),
            packetName,
            Map.of());
        return List.of(
            new ZLinkBackendActorReceived(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                requestId,
                flags,
                Message.from(ZLinkStreamHeaderCodec.encode(header)),
                true),
            new ZLinkBackendActorReceived(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                0,
                0,
                Message.from(serializer.serialize(
                    new ProbeRequest(value)).bytes()),
                false));
    }

    private static List<ZLinkBackendActorReceived> actorRequestParts(
        String actorId,
        String packetName,
        String value,
        long requestId,
        int flags,
        EnumSet<ZLinkStreamHeaderFlag> headerFlags,
        Map<String, String> metadata) {
        ZLinkBackendActorRef actorRef =
            new ZLinkBackendActorRef(RoutingId.from("entry-node"), actorId, 1);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            headerFlags,
            Optional.of(1L),
            packetName,
            metadata);
        return List.of(
            new ZLinkBackendActorReceived(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                requestId,
                flags,
                Message.from(ZLinkStreamHeaderCodec.encode(header)),
                true),
            new ZLinkBackendActorReceived(
                actorRef,
                RoutingId.from("source-node"),
                RoutingId.from("source-session"),
                Optional.empty(),
                0,
                0,
                Message.from(SERIALIZER.serialize(new ProbeRequest(value)).bytes()),
                false));
    }


    private static ReplyRecord awaitSingle(List<ReplyRecord> replies) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (replies.size() == 1) {
                return replies.get(0);
            }
            Thread.sleep(10);
        }
        assertEquals(1, replies.size());
        return replies.get(0);
    }

    private static List<ReplyRecord> awaitReplyCount(
        List<ReplyRecord> replies,
        int expected) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            if (replies.size() == expected) {
                return replies;
            }
            Thread.sleep(10);
        }
        assertEquals(expected, replies.size());
        return replies;
    }

    private static DecodedFrame decodeFrame(Message message) {
        ZLinkStreamFrameCodec.DecodedFrame frame = ZLinkStreamFrameCodec
            .tryDecode(message.toByteArray())
            .orElseThrow();
        return new DecodedFrame(
            ZLinkStreamHeaderCodec.decodeOrPlain(frame.header()),
            frame.body());
    }

    private static ProbeReply deserializeReply(DecodedFrame frame) {
        return SERIALIZER.deserialize(ZLinkEncodedPayload.from(frame.body()), ProbeReply.class);
    }

    public static final class ProbeEntrySpot implements ZLinkEntrySpot<ZLinkActor> {
        private final ZLinkEntrySpotContext context;

        public ProbeEntrySpot(ZLinkEntrySpotContext context) {
            this.context = context;
        }

        @Override
        public ZLinkEntrySpotContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeActor implements ZLinkActor {
        private final String actorId;
        private final ZLinkActorContext context;
        private final AtomicInteger handled = new AtomicInteger();

        ProbeActor(String actorId, ZLinkActorContext context) {
            this.actorId = actorId;
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        void recordHandled() {
            handled.incrementAndGet();
        }

        int handled() {
            return handled.get();
        }
    }

    public static final class ProbeActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(
                new ProbeActor(context.actorId(), context));
        }
    }

    public static final class ProbeActorRequestHandler {
        @ZLinkSpotActorRequest(packetName = "request")
        public CompletionStage<ProbeReply> handle(
            ProbeEntrySpot spot,
            ProbeActor actor,
            systems.zlink.framework.ZLinkMessageContext context,
            ProbeRequest request) {
            actor.recordHandled();
            String contentType = context.contentType()
                .filter("application/x-protobuf"::equals)
                .map(value -> ":" + value)
                .orElse("");
            return CompletableFuture.completedFuture(
                new ProbeReply(request.value() + ":"
                    + actor.context().actorId() + contentType));
        }
    }

    public static final class ProbeActorSendHandler {
        @ZLinkSpotActorSend(packetName = "follow-send")
        public CompletionStage<Void> handle(
            ProbeEntrySpot spot,
            ProbeActor actor,
            systems.zlink.framework.ZLinkMessageContext context,
            ProbeRequest request) {
            actor.recordHandled();
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProbeActorThrowHandler {
        @ZLinkSpotActorRequest(packetName = "throw")
        public CompletionStage<ProbeReply> handle(ProbeActor actor, ProbeRequest request) {
            throw new IllegalStateException(request.value());
        }
    }

    public record ProbeRequest(String value) {
    }

    public record ProbeReply(String value) {
    }

    private static final class ProbeProtobufSerializer
        implements ZLinkMessageSerializer {
        @Override
        public <T> ZLinkEncodedPayload serialize(T value) {
            String encoded = switch (value) {
                case ProbeRequest request -> "request:" + request.value();
                case ProbeReply reply -> "reply:" + reply.value();
                default -> throw new IllegalArgumentException(
                    "unsupported protobuf probe: " + value.getClass().getName());
            };
            return ZLinkEncodedPayload.from(
                encoded.getBytes(StandardCharsets.UTF_8));
        }

        @Override
        public <T> T deserialize(ZLinkEncodedPayload payload, Class<T> type) {
            String encoded = new String(payload.bytes(), StandardCharsets.UTF_8);
            if (type == ProbeRequest.class && encoded.startsWith("request:")) {
                return type.cast(new ProbeRequest(encoded.substring(8)));
            }
            if (type == ProbeReply.class && encoded.startsWith("reply:")) {
                return type.cast(new ProbeReply(encoded.substring(6)));
            }
            throw new IllegalArgumentException(
                "invalid protobuf probe payload for " + type.getName());
        }
    }

    private record DecodedFrame(ZLinkStreamHeader header, byte[] body) {
    }

    private record ReplyRecord(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid,
        long requestId,
        int flags,
        List<Message> parts) {
    }

    private record RemoteBind(
        ZLinkBackendActorRef actor,
        RoutingId sourceNodeRid,
        RoutingId sourceSessionRid) {
    }

    private static final class TestBackend
        implements ZLinkBackendAdapterProvider, ZLinkChannelBackendAdapter, ZLinkSpotBackendAdapter {
        final TestSpotNode node = new TestSpotNode();
        final TestSpot entrySpot = node.entrySpot;

        @Override public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) { return this; }
        @Override public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) { return this; }
        @Override public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) { throw new UnsupportedOperationException(); }
        @Override public ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendContext createContext() { return new TestContext(); }
        @Override public ZLinkBackendDealerSocket createDealerSocket(ZLinkBackendContext context) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendRouterSocket createRouterSocket(ZLinkBackendContext context) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendPublisherSocket createPublisherSocket(ZLinkBackendContext context) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendSubscriberSocket createSubscriberSocket(ZLinkBackendContext context) { throw new UnsupportedOperationException(); }
        @Override public ZLinkInternalSpotNode createSpotNode(ZLinkBackendContext context, ZLinkBackendSpotNodeMode mode) { return node; }
    }

    private static final class TestContext implements ZLinkBackendContext {
        @Override public void shutdown() { }
        @Override public String name() { return "test"; }
        @Override public void close() { }
    }

    private static final class TestSpotNode implements ZLinkInternalSpotNode {
        private RoutingId routingId = RoutingId.from("entry-node");
        private final TestSpot entrySpot = new TestSpot();
        private final List<ReplyRecord> noBindReplies = new CopyOnWriteArrayList<>();
        private final List<ReplyRecord> boundSessionReplies = new CopyOnWriteArrayList<>();
        private final List<RemoteBind> remoteSessionBinds = new CopyOnWriteArrayList<>();
        private Optional<ZLinkInternalSpotNode.BoundSessionRoute>
            canonicalBoundSessionRoute = Optional.of(
                new ZLinkInternalSpotNode.BoundSessionRoute(
                    RoutingId.from("source-node"),
                    1,
                    RoutingId.from("source-session"),
                    7));

        @Override public RoutingId routingId() { return routingId; }
        @Override public void setRoutingId(RoutingId routingId) {
            this.routingId = routingId;
        }
        @Override public void setPublisherRoutingId(RoutingId routingId) { }
        @Override public void setSubscriberRoutingId(RoutingId routingId) { }
        @Override public void setRouterBind(String endpoint) { }
        @Override public void setPubBind(String endpoint) { }
        @Override public void connectPeer(String endpoint) { }
        @Override public void connectPeer(RoutingId peerRid, String endpoint) { }
        @Override public void disconnectPeer(String endpoint) { }
        @Override public void disconnectPeer(RoutingId peerRid) { }
        @Override public ZLinkBackendSpotRouteBridge createRouteBridge() { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendSpot createSpot() { return new TestSpot(); }
        @Override public ZLinkBackendSpot entrySpot() { return entrySpot; }

        @Override
        public ZLinkBackendActorRef createActor(String actorId, Message createRequest) {
            createRequest.close();
            return new ZLinkBackendActorRef(routingId, actorId, 1);
        }

        @Override public ZLinkBackendActorRef actorLookup(String actorId) { return new ZLinkBackendActorRef(routingId, actorId, 1); }
        @Override public CompletionStage<ZLinkBackendActorJoinResult> joinActor(ZLinkBackendActorRef actor, RoutingId targetNodeRid, String targetSpotId, List<Message> parts, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<ZLinkBackendActorJoinEntrySpotResult> joinActorEntrySpot(ZLinkBackendActorRef actor, RoutingId targetNodeRid, Message request, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<List<Message>> leaveActor(ZLinkBackendActorRef actor, String currentSpotId, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<Void> destroyActor(ZLinkBackendActorRef actor, Duration timeout) { throw new UnsupportedOperationException(); }

        @Override
        public boolean sendActorBoundSession(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) {
            boundSessionReplies.add(new ReplyRecord(actor, null, null, 0, 0, copyParts(parts)));
            return true;
        }

        @Override
        public void replyActorNoBind(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid,
            long requestId,
            int flags,
            List<Message> parts) {
            noBindReplies.add(new ReplyRecord(
                actor,
                sourceNodeRid,
                sourceSessionRid,
                requestId,
                flags,
                copyParts(parts)));
        }

        @Override public boolean sendToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<List<Message>> requestToActor(ZLinkBackendActorRef actor, List<Message> parts, SendFlags flags, Duration timeout) { throw new UnsupportedOperationException(); }
        @Override public boolean forwardActorBoundSession(ZLinkBackendActorRef actor, RoutingId sourceNodeRid, RoutingId sourceSessionRid, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }

        @Override
        public void bindRemoteActorBoundSession(
            ZLinkBackendActorRef actor,
            RoutingId sourceNodeRid,
            RoutingId sourceSessionRid) {
            remoteSessionBinds.add(new RemoteBind(actor, sourceNodeRid, sourceSessionRid));
        }

        @Override
        public Optional<ZLinkInternalSpotNode.BoundSessionRoute>
            boundSessionRoute(ZLinkBackendActorRef actor) {
            return canonicalBoundSessionRoute;
        }

        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) { }
        @Override public String name() { return "test-node"; }
        @Override public void close() { }
    }

    private static final class TestSpot implements ZLinkBackendSpot {
        private String routingId = "entry-spot";
        private ZLinkBackendSpotDispatchHandler handler;
        private final ConcurrentLinkedQueue<ZLinkBackendReceived> routes =
            new ConcurrentLinkedQueue<>();
        private final AtomicLong routeRequestSequence = new AtomicLong(1);

        void raiseActorReadable(List<ZLinkBackendActorReceived> actorMessages) {
            handler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ACTOR_READABLE,
                actorMessages));
        }

        @Override public String spotId() { return routingId; }
        @Override public void setRoutingId(String spotId) { this.routingId = spotId; }
        @Override public void setSubscription(String topic) { }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return null; }
        @Override public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) {
            return routes.poll();
        }
        @Override public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public CompletionStage<Void> publishAsync(String channelName, String topic, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override
        public CompletionStage<Void> sendToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts) {
            routes.add(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.of(RoutingId.from("message-follow-source")),
                Optional.of("message-follow-source-spot"),
                Optional.empty(),
                new byte[0],
                copyParts(parts),
                null,
                () -> { }));
            handler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ROUTED_READABLE,
                List.of()));
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<ZLinkBackendReceived> requestToSpot(
            RoutingId targetNodeRid,
            String spotId,
            long spotGeneration,
            List<Message> parts,
            Duration timeout) {
            CompletableFuture<ZLinkBackendReceived> result =
                new CompletableFuture<>();
            long requestSequence = routeRequestSequence.getAndIncrement();
            routes.add(new ZLinkBackendReceived(
                ZLinkBackendRequestResult.OK,
                Optional.of(RoutingId.from("message-follow-source")),
                Optional.of("message-follow-source-spot"),
                Optional.of(requestSequence),
                new byte[0],
                copyParts(parts),
                replyParts -> result.complete(new ZLinkBackendReceived(
                    ZLinkBackendRequestResult.OK,
                    Optional.of(RoutingId.from("entry-node")),
                    Optional.of(routingId),
                    Optional.of(requestSequence),
                    copyParts(replyParts))),
                () -> { }));
            handler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ROUTED_READABLE,
                List.of()));
            return result;
        }
        @Override public void onDispatchEvent(ZLinkBackendSpotDispatchHandler handler) { this.handler = handler; }
        @Override public ZLinkBackendActorJoinRequest recvActorJoin(ZLinkBackendRecvMode mode) { return null; }
        @Override public void replyActorJoin(ZLinkBackendActorJoinRequest request, int joinResultCode, List<Message> parts) { throw new UnsupportedOperationException(); }
        @Override public ZLinkBackendActorLifecycleEvent recvActorLifecycle(ZLinkBackendRecvMode mode) { return null; }
        @Override public String name() { return "test-spot"; }
        @Override public void close() { }
    }

    private static List<Message> copyParts(List<Message> parts) {
        List<Message> copies = new ArrayList<>();
        for (Message part : parts) {
            copies.add(Message.from(part));
        }
        return copies;
    }
}
