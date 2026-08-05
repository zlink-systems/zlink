package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkEncodedPayload;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowObserver;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.messaging.ZLinkMessage;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestCallback;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.handlers.ZLinkSpotActorRequest;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class EntrySpotActorDispatchTests {
    private static final int NO_BIND = 1;
    private static final ZLinkJsonMessageSerializer SERIALIZER = new ZLinkJsonMessageSerializer();

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
    void entrySpotActorDispatchKeepsInboundLeaseUntilActorTurnCompletes() throws Exception {
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            ZLinkInboundDispatchBudget budget = new ZLinkInboundDispatchBudget(0);
            try (ZLinkInboundDispatchBudget.Lease lease = budget.track(1)) {
                backend.entrySpot.raiseActorReadable(actorRequestParts(
                    "actor-a", "request", "lease", 45, NO_BIND, lease));

                ReplyRecord reply = awaitSingle(backend.node.noBindReplies);
                DecodedFrame frame = decodeFrame(reply.parts().get(0));
                assertEquals(ZLinkStreamMessageKind.RESPONSE, frame.header().kind());
                assertEquals("lease:actor-a", deserializeReply(frame).value());
                assertEquals(0, budget.snapshot().pendingPayloadBytes());
            }
        }
    }

    @Test
    void entrySpotActorDispatchNoBindHandlerExceptionRepliesNoBindError() throws Exception {
        CapturingFlowObserver.clear();
        TestBackend backend = startBackend();
        try (ZLinkFrameworkRuntime runtime = startRuntime(backend)) {
            runtime.actorManager().create("actor-a", "probe").submit()
                .toCompletableFuture().get(5, TimeUnit.SECONDS);

            backend.entrySpot.raiseActorReadable(actorRequestParts("actor-a", "throw", "boom", 43, NO_BIND));

            ReplyRecord reply = awaitSingle(backend.node.noBindReplies);
            DecodedFrame frame = decodeFrame(reply.parts().get(0));
            assertEquals(ZLinkStreamMessageKind.ERROR, frame.header().kind());
            assertTrue(new String(frame.body(), java.nio.charset.StandardCharsets.UTF_8).contains("boom"));
            assertTrue(backend.node.boundSessionReplies.isEmpty());
            assertEquals(0, backend.node.remoteSessionBinds.size());

            ZLinkMessageFlowEvent event = awaitHandlerExceptionFlow();
            assertEquals(ZLinkDispatchErrorSurface.SPOT_ACTOR, event.surface());
            assertEquals(ZLinkDispatchMessageKind.ACTOR_REQUEST, event.messageKind());
            assertEquals(ZLinkDispatchErrorReason.HANDLER_EXCEPTION, event.errorReason());
            assertEquals(ZLinkDispatchErrorAction.REPLY_ERROR, event.errorAction());
            assertEquals("throw", event.packetName());
            assertEquals("actor-a", event.actorId());
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
            assertTrue(new String(frame.body(), java.nio.charset.StandardCharsets.UTF_8)
                .contains("not registered locally"));
            assertTrue(backend.node.boundSessionReplies.isEmpty());
            assertEquals(0, backend.node.remoteSessionBinds.size());
        }
    }

    private static TestBackend startBackend() {
        TestBackend backend = new TestBackend();
        return backend;
    }

    private static ZLinkFrameworkRuntime startRuntime(TestBackend backend) {
        DefaultZLinkFrameworkOptions options = new DefaultZLinkFrameworkOptions();
        options.setDefaultRequestTimeout(Duration.ofSeconds(1));
        options.addHandlersFromPackageOf(EntrySpotActorDispatchTests.class);
        options.configureDispatch().setMessageFlowObserver(new CapturingFlowObserver());
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

    private static List<ZLinkBackendActorReceived> actorRequestParts(
        String actorId,
        String packetName,
        String value,
        long requestId,
        int flags,
        ZLinkInboundDispatchBudget.Lease bodyLease) {
        ZLinkBackendActorRef actorRef =
            new ZLinkBackendActorRef(RoutingId.from("entry-node"), actorId, 1);
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(1L),
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
                Message.from(SERIALIZER.serialize(new ProbeRequest(value)).bytes()),
                false,
                new byte[0],
                null,
                bodyLease));
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

    private static ZLinkMessageFlowEvent awaitHandlerExceptionFlow() throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (System.nanoTime() < deadline) {
            for (ZLinkMessageFlowEvent event : CapturingFlowObserver.events) {
                if (event.errorReason() == ZLinkDispatchErrorReason.HANDLER_EXCEPTION) {
                    return event;
                }
            }
            Thread.sleep(10);
        }
        assertFalse(CapturingFlowObserver.events.isEmpty());
        return CapturingFlowObserver.events.get(0);
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

        ProbeActor(String actorId, ZLinkActorContext context) {
            this.actorId = actorId;
            this.context = context;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
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
        public CompletionStage<ProbeReply> handle(ProbeActor actor, ProbeRequest request) {
            return CompletableFuture.completedFuture(
                new ProbeReply(request.value() + ":" + actor.context().actorId()));
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

    private static final class CapturingFlowObserver implements ZLinkMessageFlowObserver {
        static final List<ZLinkMessageFlowEvent> events = new CopyOnWriteArrayList<>();

        static void clear() {
            events.clear();
        }

        @Override
        public CompletionStage<Void> onMessageFlow(ZLinkMessageFlowEvent flow) {
            events.add(flow);
            return CompletableFuture.completedFuture(null);
        }
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

        @Override public void closeActorBoundSession(ZLinkBackendActorRef actor, Duration timeout) { }
        @Override public String name() { return "test-node"; }
        @Override public void close() { }
    }

    private static final class TestSpot implements ZLinkBackendSpot {
        private String routingId = "entry-spot";
        private ZLinkBackendSpotDispatchHandler handler;

        void raiseActorReadable(List<ZLinkBackendActorReceived> actorMessages) {
            handler.handle(new ZLinkBackendSpotDispatchInfo(
                ZLinkBackendSpotDispatchEvent.ACTOR_READABLE,
                actorMessages));
        }

        @Override public String spotId() { return routingId; }
        @Override public void setRoutingId(String spotId) { this.routingId = spotId; }
        @Override public void setSubscription(String topic) { }
        @Override public ZLinkBackendTopicMessage subscribe(ZLinkBackendRecvMode mode) { return null; }
        @Override public ZLinkBackendReceived recvRoute(ZLinkBackendRecvMode mode) { return null; }
        @Override public boolean publish(String channelName, String topic, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public boolean sendToSpot(RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts, SendFlags flags) { throw new UnsupportedOperationException(); }
        @Override public boolean requestToSpot(RoutingId targetNodeRid, String spotId, long spotGeneration, List<Message> parts, ZLinkBackendRequestCallback callback, SendFlags flags, Duration timeout) { throw new UnsupportedOperationException(); }
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
