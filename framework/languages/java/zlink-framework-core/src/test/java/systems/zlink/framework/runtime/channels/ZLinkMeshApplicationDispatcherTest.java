package systems.zlink.framework.runtime.channels;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeUnit;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.OwnerKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReadyRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.channels.ZLinkRouteMessageContext;
import systems.zlink.framework.channels.ZLinkRouteSendHandler;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.channels.ZLinkSendHandler;
import systems.zlink.framework.channels.ZLinkRequestHandler;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;

final class ZLinkMeshApplicationDispatcherTest {
    @BeforeEach
    void resetHandlers() {
        NodeHandler.received = new CompletableFuture<>();
        ChannelHandler.received = new CompletableFuture<>();
        NodeHandler.metadata = new CompletableFuture<>();
        NodeHandler.contentType = new CompletableFuture<>();
        ChannelHandler.metadata = new CompletableFuture<>();
        ScannedChannelHandler.received = new CompletableFuture<>();
        GatedNodeHandler.started = new CompletableFuture<>();
        GatedNodeHandler.release = new CompletableFuture<>();
        GatedNodeHandler.completedCount = new java.util.concurrent.atomic.AtomicInteger();
        GatedNodeHandler.completed = new CompletableFuture<>();
        ProtocolRequestHandler.received = new CompletableFuture<>();
    }

    @Test
    void dispatchesNodeSendThroughTypedRouteHandler() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-node");
        mesh.addRouteSendHandler(NodeHandler.class, String.class);
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);

        dispatcher.accept(record(
            RecordKind.NODE_SEND,
            null,
            "node-value",
            Map.of("trace-id", "node-trace")));

        assertEquals("node-value@source-node",
            NodeHandler.received.get(2, TimeUnit.SECONDS));
        assertEquals(
            Map.of("trace-id", "node-trace"),
            NodeHandler.metadata.get(2, TimeUnit.SECONDS));
    }

    @Test
    void dispatchesWireContentTypeFrameWhenRecordOmitsDecodedContentType()
        throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-wire-content-type");
        mesh.addRouteSendHandler(NodeHandler.class, String.class);
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);

        dispatcher.accept(recordWithWireContentType("wire-value", "application/json"));

        assertEquals(
            "application/json",
            NodeHandler.contentType.get(2, TimeUnit.SECONDS));
    }

    @Test
    void dispatchesChannelSendThroughTypedChannelHandler() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-channel");
        mesh.channelName("play").server()
            .addSendHandler(ChannelHandler.class, String.class);
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);

        dispatcher.accept(record(
            RecordKind.CHANNEL_SEND,
            "play",
            "channel-value",
            Map.of("tenant", "blue")));

        assertEquals("channel-value@play",
            ChannelHandler.received.get(2, TimeUnit.SECONDS));
        assertEquals(
            Map.of("tenant", "blue"),
            ChannelHandler.metadata.get(2, TimeUnit.SECONDS));
    }

    @Test
    void routeMeshApplicationDispatchWritesMessageFlowFile() throws Exception {
        Path flowFile = Files.createTempFile("zlink-route-mesh-flow", ".log");
        Files.deleteIfExists(flowFile);
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-flow");
        mesh.addRouteSendHandler(NodeHandler.class, String.class);
        ZLinkFrameworkRegistration framework = new ZLinkFrameworkRegistration();
        framework.dispatchOptions()
            .messageFlow(systems.zlink.framework.configuration.ZLinkMessageFlowLogMode
                .KEY_TRANSITIONS)
            .traceLogFile(flowFile.toString())
            .traceLabel("mesh");
        ZLinkMeshApplicationDispatcher dispatcher =
            new ZLinkMeshApplicationDispatcher(
                mesh,
                new ZLinkStringMessageSerializer(),
                framework,
                ZLinkHandlerActivator.reflection(),
                (token, parts) -> {
                    throw new AssertionError("send dispatch must not reply");
                });

        dispatcher.accept(record(RecordKind.NODE_SEND, null, "flow-value"));

        assertEquals(
            "flow-value@source-node",
            NodeHandler.received.get(2, TimeUnit.SECONDS));
        String content = "";
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
        while (System.nanoTime() < deadline) {
            if (Files.exists(flowFile)) {
                content = Files.readString(flowFile);
                if (content.contains("outcome=DISPATCHED")) {
                    break;
                }
            }
            Thread.sleep(1);
        }
        assertTrue(content.contains("outcome=RECEIVED"), content);
        assertTrue(content.contains("outcome=DISPATCHED"), content);
        assertTrue(content.contains("label=mesh"), content);
        Files.deleteIfExists(flowFile);
    }

    @Test
    void rejectsUnknownIncomingContentTypeBeforeRequestHandler() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-unknown-content-type");
        mesh.addRouteRequestHandler(ProtocolRequestHandler.class, String.class, String.class);
        CompletableFuture<ZLinkFrameworkErrorKind> replyKind = new CompletableFuture<>();
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);

        dispatcher.accept(record(
            RecordKind.NODE_REQUEST,
            null,
            "valid-json-body",
            Map.of(),
            "application/x-unregistered",
            parts -> replyKind.complete(ZLinkFrameworkErrorReply.kind(parts))));

        assertEquals(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            replyKind.get(2, TimeUnit.SECONDS));
        assertFalse(ProtocolRequestHandler.received.isDone());
    }

    @Test
    void scannedChannelHandlerUsesMatchingChannelNameWithoutManualGroupMapping()
        throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-scanned-channel");
        mesh.channelName("play");
        ZLinkFrameworkRegistration framework = new ZLinkFrameworkRegistration();
        framework.handlerPackageMarkers().add(ZLinkMeshApplicationDispatcherTest.class);
        ZLinkMeshApplicationDispatcher dispatcher = new ZLinkMeshApplicationDispatcher(
            mesh,
            new ZLinkStringMessageSerializer(),
            framework,
            ZLinkHandlerActivator.reflection(),
            (token, parts) -> {
                throw new AssertionError("send dispatch must not reply");
            });

        dispatcher.accept(record(RecordKind.CHANNEL_SEND, "play", "scanned"));

        assertEquals("scanned@play",
            ScannedChannelHandler.received.get(2, TimeUnit.SECONDS));
    }

    @Test
    void sealedMeshRejectsApplicationBeforeHandlerClaim() {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-draining");
        mesh.addRouteSendHandler(NodeHandler.class, String.class);
        ZLinkMeshDrainCoordinator drains =
            new ZLinkMeshDrainCoordinator(List.of("game"));
        drains.sealAndAwaitZero("game");
        ZLinkMeshApplicationDispatcher dispatcher = new ZLinkMeshApplicationDispatcher(
            mesh,
            new ZLinkStringMessageSerializer(),
            new ZLinkFrameworkRegistration(),
            ZLinkHandlerActivator.reflection(),
            (token, parts) -> parts.forEach(Message::close),
            drains);

        dispatcher.accept(record(RecordKind.NODE_SEND, null, "rejected"));

        assertEquals(false, NodeHandler.received.isDone());
    }

    @Test
    void localNodeSendOwnsPayloadAndCompletesAdmissionBeforeHandler() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-local-node-send");
        mesh.addRouteSendHandler(GatedNodeHandler.class, String.class);
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);
        List<Message> parts = List.of(
            Message.from("String".getBytes(StandardCharsets.UTF_8)),
            Message.from("owned-value".getBytes(StandardCharsets.UTF_8)));

        Integer status = dispatcher.submitLocalNodeSend(
            RoutingId.from("source-node"), new byte[0], parts);
        parts.forEach(Message::close);

        assertEquals(0, status);
        assertEquals("owned-value@source-node",
            GatedNodeHandler.started.get(2, TimeUnit.SECONDS));
        assertFalse(GatedNodeHandler.release.isDone());
        assertEquals(0, GatedNodeHandler.completedCount.get());

        GatedNodeHandler.release.complete(null);

        assertEquals(1, GatedNodeHandler.completed.get(2, TimeUnit.SECONDS));
    }

    @Test
    void localNodeSendUsesBoundedPendingCapacityAndEmitsReadyOnce() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-local-node-capacity");
        mesh.configureRouterSocket().setReceiveHighWaterMark(1);
        mesh.addRouteSendHandler(GatedNodeHandler.class, String.class);
        ZLinkMeshApplicationDispatcher dispatcher = dispatcher(mesh);
        CompletableFuture<Void> capacityAvailable = new CompletableFuture<>();
        dispatcher.setLocalNodeReadyHandler(() -> capacityAvailable.complete(null));

        assertEquals(
            0,
            submitLocal(dispatcher, "String", "active"));
        GatedNodeHandler.started.get(2, TimeUnit.SECONDS);
        assertEquals(
            0,
            submitLocal(dispatcher, "String", "pending"));
        assertEquals(
            1,
            submitLocal(dispatcher, "String", "rejected"));

        GatedNodeHandler.release.complete(null);

        capacityAvailable.get(2, TimeUnit.SECONDS);
        assertEquals(
            0,
            submitLocal(dispatcher, "String", "after-ready"));
    }

    @Test
    void localNodeSendMapsMissingHandlerAndSealedAdmission() {
        MeshNodeRegistration missingMesh = new MeshNodeRegistration("missing");
        missingMesh.listen("inproc://mesh-local-node-missing");
        ZLinkMeshApplicationDispatcher missing = dispatcher(missingMesh);
        assertEquals(
            4,
            submitLocal(missing, "String", "missing"));

        MeshNodeRegistration sealedMesh = new MeshNodeRegistration("sealed");
        sealedMesh.listen("inproc://mesh-local-node-sealed");
        sealedMesh.addRouteSendHandler(GatedNodeHandler.class, String.class);
        ZLinkMeshDrainCoordinator drains = new ZLinkMeshDrainCoordinator(List.of("sealed"));
        drains.seal("sealed");
        ZLinkMeshApplicationDispatcher sealed = new ZLinkMeshApplicationDispatcher(
            sealedMesh,
            new ZLinkStringMessageSerializer(),
            new ZLinkFrameworkRegistration(),
            ZLinkHandlerActivator.reflection(),
            (token, parts) -> parts.forEach(Message::close),
            drains);

        assertEquals(
            5,
            submitLocal(sealed, "String", "sealed"));
        assertFalse(GatedNodeHandler.started.isDone());
    }

    @Test
    void localNodeSendReleasesDrainClaimAfterHandlerFailure() throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-local-node-failure");
        mesh.addRouteSendHandler(FailingNodeHandler.class, String.class);
        ZLinkMeshDrainCoordinator drains = new ZLinkMeshDrainCoordinator(List.of("game"));
        ZLinkMeshApplicationDispatcher dispatcher = new ZLinkMeshApplicationDispatcher(
            mesh,
            new ZLinkStringMessageSerializer(),
            new ZLinkFrameworkRegistration(),
            ZLinkHandlerActivator.reflection(),
            (token, parts) -> parts.forEach(Message::close),
            drains);

        assertEquals(
            0,
            submitLocal(dispatcher, "String", "failure"));

        drains.sealAndAwaitZero("game").toCompletableFuture().get(2, TimeUnit.SECONDS);
        assertTrue(drains.awaitZero("game").toCompletableFuture().isDone());
    }

    @Test
    void attachedRawMailboxLeaseIsNotCountedAgainByTheApplicationDispatcher()
        throws Exception {
        MeshNodeRegistration mesh = new MeshNodeRegistration("game");
        mesh.listen("inproc://mesh-dispatch-attached-lease");
        mesh.addRouteSendHandler(GatedNodeHandler.class, String.class);
        ZLinkInboundDispatchBudget budget =
            new ZLinkInboundDispatchBudget(10);
        ZLinkMeshApplicationDispatcher dispatcher =
            new ZLinkMeshApplicationDispatcher(
                mesh,
                new ZLinkStringMessageSerializer(),
                new ZLinkFrameworkRegistration(),
                ZLinkHandlerActivator.reflection(),
                (token, parts) -> parts.forEach(Message::close),
                null,
                budget);
        ZLinkInboundDispatchBudget.Lease mailboxLease = budget.track(7);

        dispatcher.accept(record(
            RecordKind.NODE_SEND,
            null,
            "mailbox-value",
            Map.of(),
            null,
            null,
            mailboxLease));

        assertEquals(
            7,
            budget.snapshot().pendingPayloadBytes());
        GatedNodeHandler.started.get(2, TimeUnit.SECONDS);
        assertEquals(7, budget.snapshot().activePayloadBytes());

        GatedNodeHandler.release.complete(null);
        long deadline = System.nanoTime()
            + TimeUnit.SECONDS.toNanos(2);
        while (budget.snapshot().pendingPayloadBytes() != 0
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        assertEquals(0, budget.snapshot().pendingPayloadBytes());
    }

    private static Integer submitLocal(
        ZLinkMeshApplicationDispatcher dispatcher,
        String packetName,
        String value) {
        List<Message> parts = List.of(
            Message.from(packetName.getBytes(StandardCharsets.UTF_8)),
            Message.from(value.getBytes(StandardCharsets.UTF_8)));
        try {
            return dispatcher.submitLocalNodeSend(
                RoutingId.from("source-node"), new byte[0], parts);
        } finally {
            parts.forEach(Message::close);
        }
    }

    private static ZLinkMeshApplicationDispatcher dispatcher(MeshNodeRegistration mesh) {
        return new ZLinkMeshApplicationDispatcher(
            mesh,
            new ZLinkStringMessageSerializer(),
            new ZLinkFrameworkRegistration(),
            ZLinkHandlerActivator.reflection(),
            (token, parts) -> {
                throw new AssertionError("send dispatch must not reply");
            });
    }

    private static ZLinkMeshDispatchRecord record(
        RecordKind kind,
        String channelName,
        String value) {
        return record(kind, channelName, value, Map.of());
    }

    private static ZLinkMeshDispatchRecord record(
        RecordKind kind,
        String channelName,
        String value,
        Map<String, String> metadata) {
        return record(kind, channelName, value, metadata, null, null);
    }

    private static ZLinkMeshDispatchRecord record(
        RecordKind kind,
        String channelName,
        String value,
        Map<String, String> metadata,
        String contentType,
        java.util.function.Consumer<List<Message>> reply) {
        return record(kind, channelName, value, metadata, contentType, reply, null);
    }

    private static ZLinkMeshDispatchRecord record(
        RecordKind kind,
        String channelName,
        String value,
        Map<String, String> metadata,
        String contentType,
        java.util.function.Consumer<List<Message>> reply,
        ZLinkInboundDispatchBudget.Lease inboundDispatchLease) {
        RoutingId source = RoutingId.from("source-node");
        ReadyRecord owner = new ReadyRecord(OwnerKind.NODE, 1, null, null);
        ReceiveRecord receive = new ReceiveRecord(
            kind,
            1,
            source,
            null,
            null,
            null,
            null,
            null,
            channelName,
            null,
            contentType,
            ZLinkApplicationMetadata.copyOf(metadata).encode(),
            0,
            0,
            0,
            2);
        return new ZLinkMeshDispatchRecord(
            owner,
            receive,
            List.of(
                Message.from("String".getBytes(StandardCharsets.UTF_8)),
                Message.from(value.getBytes(StandardCharsets.UTF_8))),
            reply,
            inboundDispatchLease);
    }

    private static ZLinkMeshDispatchRecord recordWithWireContentType(
        String value,
        String contentType) {
        RoutingId source = RoutingId.from("source-node");
        ReadyRecord owner = new ReadyRecord(OwnerKind.NODE, 1, null, null);
        ReceiveRecord receive = new ReceiveRecord(
            RecordKind.NODE_SEND,
            1,
            source,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            null,
            ZLinkApplicationMetadata.copyOf(Map.of()).encode(),
            0,
            0,
            0,
            2);
        return new ZLinkMeshDispatchRecord(
            owner,
            receive,
            List.of(
                Message.from("String".getBytes(StandardCharsets.UTF_8)),
                Message.from(value.getBytes(StandardCharsets.UTF_8)),
                ZLinkChannelContentTypeFrame.encode(contentType)),
            null,
            null);
    }

    public static final class NodeHandler implements ZLinkRouteSendHandler<String> {
        private static CompletableFuture<String> received;
        private static CompletableFuture<Map<String, String>> metadata;
        private static CompletableFuture<String> contentType;

        public NodeHandler() {
        }

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkRouteMessageContext context) {
            received.complete(message + "@" + context.sourceNodeRid());
            metadata.complete(context.metadata());
            contentType.complete(context.contentType().orElse(null));
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class ProtocolRequestHandler
        implements systems.zlink.framework.channels.ZLinkRouteRequestHandler<String, String> {
        private static CompletableFuture<String> received = new CompletableFuture<>();

        @Override
        public CompletionStage<String> handle(
            String message,
            ZLinkRouteMessageContext context) {
            received.complete(message);
            return CompletableFuture.completedFuture("reply");
        }
    }

    public static final class GatedNodeHandler implements ZLinkRouteSendHandler<String> {
        private static CompletableFuture<String> started;
        private static CompletableFuture<Void> release;
        private static java.util.concurrent.atomic.AtomicInteger completedCount;
        private static CompletableFuture<Integer> completed;

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkRouteMessageContext context) {
            started.complete(message + "@" + context.sourceNodeRid());
            return release.whenComplete((ignored, error) ->
                completed.complete(completedCount.incrementAndGet()));
        }
    }

    public static final class FailingNodeHandler implements ZLinkRouteSendHandler<String> {
        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkRouteMessageContext context) {
            throw new IllegalStateException("expected failure");
        }
    }

    public static final class ChannelHandler implements ZLinkSendHandler<String> {
        private static CompletableFuture<String> received;
        private static CompletableFuture<Map<String, String>> metadata;

        public ChannelHandler() {
        }

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkMessageContext context) {
            received.complete(message + "@" + context.channelName().orElse(""));
            metadata.complete(context.metadata());
            return CompletableFuture.completedFuture(null);
        }
    }

    @ZLinkHandlerGroup("play")
    public static final class ScannedChannelHandler implements ZLinkSendHandler<String> {
        private static CompletableFuture<String> received;

        public ScannedChannelHandler() {
        }

        @Override
        public CompletionStage<Void> handle(
            String message,
            ZLinkMessageContext context) {
            received.complete(message + "@" + context.channelName().orElse(""));
            return CompletableFuture.completedFuture(null);
        }
    }
}
