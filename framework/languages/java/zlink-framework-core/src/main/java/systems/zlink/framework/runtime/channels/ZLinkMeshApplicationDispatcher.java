package systems.zlink.framework.runtime.channels;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.binding.spot.Dispatch;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReplyToken;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerSurface;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;

/**
 * Dispatches MeshNode application records through the framework's typed
 * serializer and handler activation path.
 */
public final class ZLinkMeshApplicationDispatcher
    implements ZLinkMeshApplicationReceiver {
    @FunctionalInterface
    interface ReplySender {
        void send(ReplyToken token, List<Message> parts);
    }

    private static final String NODE_NAMESPACE = "";

    private final ZLinkChannelHandlerInvoker invoker;
    private final ReplySender replies;
    private final String meshName;
    private final ZLinkMeshDrainCoordinator drains;
    private final ZLinkInboundDispatchBudget inboundDispatchBudget;
    private final ZLinkMessageFlowTracer flow;
    private AutoCloseable inboundCapacityRegistration;
    private final Map<String, Namespace> namespaces = new HashMap<>();

    public ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory) {
        this(mesh, serializer, framework, handlerFactory,
            (token, parts) -> Dispatch.reply(token, parts, SendFlags.NONE), null,
            new ZLinkInboundDispatchBudget(0));
    }

    public ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ZLinkMeshDrainCoordinator drains) {
        this(mesh, serializer, framework, handlerFactory,
            (token, parts) -> Dispatch.reply(token, parts, SendFlags.NONE), drains,
            new ZLinkInboundDispatchBudget(0));
    }

    public ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ZLinkMeshDrainCoordinator drains,
        ZLinkInboundDispatchBudget inboundDispatchBudget) {
        this(mesh, serializer, framework, handlerFactory,
            (token, parts) -> Dispatch.reply(token, parts, SendFlags.NONE),
            drains, inboundDispatchBudget);
    }

    ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ReplySender replies) {
        this(mesh, serializer, framework, handlerFactory, replies, null);
    }

    ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ReplySender replies,
        ZLinkMeshDrainCoordinator drains) {
        this(mesh, serializer, framework, handlerFactory, replies, drains,
            new ZLinkInboundDispatchBudget(0));
    }

    ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ReplySender replies,
        ZLinkMeshDrainCoordinator drains,
        ZLinkInboundDispatchBudget inboundDispatchBudget) {
        Objects.requireNonNull(mesh, "mesh");
        Objects.requireNonNull(framework, "framework");
        this.meshName = mesh.meshName();
        this.drains = drains;
        this.inboundDispatchBudget = Objects.requireNonNull(
            inboundDispatchBudget, "inboundDispatchBudget");
        this.flow = new ZLinkMessageFlowTracer(
            framework.dispatchOptions(),
            handlerFactory,
            framework.handlerExecutor());
        this.replies = Objects.requireNonNull(replies, "replies");
        this.invoker = new ZLinkChannelHandlerInvoker(
            Objects.requireNonNull(serializer, "serializer"),
            framework.codecs(),
            Objects.requireNonNull(handlerFactory, "handlerFactory"),
            framework.handlerExecutor(),
            framework.suspendHandlerInvokers(),
            framework.filters(),
            meshName);
        ZLinkScannedHandlerCatalog scannedHandlers =
            ZLinkHandlerScanner.scan(framework.handlerPackageMarkers());
        //  The receive HWM is an accounted byte count. This capacity is a message
        //  count, so saturate rather than wrap when the HWM exceeds int range.
        long receiveHighWaterMark = mesh.configureRouterSocket().receiveHighWaterMark();
        int localPendingCapacity =
            receiveHighWaterMark > 0
                ? (int) Math.min(receiveHighWaterMark, Integer.MAX_VALUE)
                : 4096;
        // The socket HWM describes pending application records. The serial
        // queue accounts the active record as well, so retain one execution
        // slot in addition to the configured pending capacity.
        int localQueueCapacity = localPendingCapacity == Integer.MAX_VALUE
            ? Integer.MAX_VALUE
            : localPendingCapacity + 1;
        namespaces.put(
            NODE_NAMESPACE,
            routeNamespace(
                mesh.nodeHandlers(),
                localQueueCapacity,
                framework.serialExecutor()));
        mesh.channelHandlers().forEach((name, handlers) ->
            namespaces.put(name, channelNamespace(
                name,
                handlers,
                mesh.channelHandlerGroups().getOrDefault(name, List.of()),
                scannedHandlers,
                framework.serialExecutor())));
    }

    @Override
    public void accept(ZLinkMeshDispatchRecord record) {
        Objects.requireNonNull(record, "record");
        ZLinkMeshDrainCoordinator.Claim claim = drains == null
            ? null
            : drains.tryClaim(meshName);
        if (drains != null && claim == null) {
            reject(record, "RouteMesh application admission is sealed", null);
            return;
        }
        RecordKind kind = record.receive().kind();
        Namespace namespace = namespace(kind, record.receive().channelName());
        if (namespace == null || record.parts().size() < 2) {
            reject(record, "MeshNode message has no registered handler namespace or payload", claim);
            return;
        }

        String packetName = record.parts().get(0).toUtf8String();
        Message payload = record.parts().get(1);
        ZLinkInboundDispatchBudget.Lease lease =
            record.inboundDispatchLease();
        if (lease == null) {
            lease = inboundDispatchBudget.track(payload.size());
        }
        String contentType = record.receive().contentType() != null
            ? record.receive().contentType()
            : ZLinkChannelContentTypeFrame.decode(record.parts());
        Map<String, String> metadata;
        try {
            metadata = ZLinkApplicationMetadata.decode(
                record.receive().applicationMetadata());
        } catch (IllegalArgumentException error) {
            reject(record, error.getMessage(), claim, lease);
            return;
        }
        switch (kind) {
            case NODE_SEND, CHANNEL_SEND ->
                dispatchSend(
                    record, namespace, packetName, payload, metadata, contentType, claim,
                    lease);
            case NODE_REQUEST, CHANNEL_REQUEST ->
                dispatchRequest(
                    record, namespace, packetName, payload, metadata, contentType, claim,
                    lease);
            default -> closeRecord(record, claim, lease);
        }
    }

    @Override
    public void setLocalNodeReadyHandler(Runnable handler) {
        namespaces.get(NODE_NAMESPACE).sendQueue.onCapacityAvailable(handler);
    }

    @Override
    public ZLinkInboundDispatchBudget applicationDispatchBudget() {
        return inboundDispatchBudget;
    }

    @Override
    public boolean canReceiveApplication() {
        return inboundDispatchBudget.canStartApplicationReceive();
    }

    @Override
    public void setApplicationReceiveReadyHandler(Runnable handler) {
        try {
            if (inboundCapacityRegistration != null) {
                inboundCapacityRegistration.close();
            }
            inboundCapacityRegistration = inboundDispatchBudget.onCapacityAvailable(handler);
        } catch (Exception error) {
            throw new IllegalStateException(
                "failed to install inbound dispatch capacity handler", error);
        }
    }

    @Override
    public int submitLocalNodeSend(
        systems.zlink.contracts.core.RoutingId sourceNodeRid,
        byte[] metadataBytes,
        List<Message> parts) {
        Namespace namespace = namespaces.get(NODE_NAMESPACE);
        if (namespace == null || parts.size() < 2) {
            return ZLinkOneWayCalls.TARGET_NOT_FOUND;
        }
        String packetName = parts.get(0).toUtf8String();
        ChannelRouteSendHandlerRegistration route = namespace.routeSends.get(packetName);
        if (route == null) {
            return ZLinkOneWayCalls.TARGET_NOT_FOUND;
        }
        ZLinkMeshDrainCoordinator.Claim claim = drains == null
            ? null
            : drains.tryClaim(meshName);
        if (drains != null && claim == null) {
            return ZLinkOneWayCalls.SHUTDOWN;
        }

        Message payload = null;
        ZLinkInboundDispatchBudget.Lease lease = null;
        try {
            payload = Message.from(parts.get(1));
            lease = inboundDispatchBudget.track(payload.size());
            Map<String, String> metadata = ZLinkApplicationMetadata.decode(metadataBytes);
            String contentType = ZLinkChannelContentTypeFrame.decode(parts);
            Message ownedPayload = payload;
            ZLinkInboundDispatchBudget.Lease ownedLease = lease;
            boolean accepted = namespace.sendQueue.tryEnqueueWithPayloadBytes(
                payload.size(), () -> {
                try {
                    ownedLease.handlerStarted();
                    return invoker.executeHandler(() -> invoker.invokeRouteSendHandler(
                            null, route, sourceNodeRid, ownedPayload, metadata, contentType))
                        .whenComplete((ignored, error) -> {
                            ownedPayload.close();
                            ownedLease.close();
                            if (claim != null) {
                                claim.close();
                            }
                        });
                } catch (RuntimeException failure) {
                    ownedPayload.close();
                    ownedLease.close();
                    if (claim != null) {
                        claim.close();
                    }
                    return java.util.concurrent.CompletableFuture.failedFuture(failure);
                }
            });
            if (!accepted) {
                payload.close();
                lease.close();
                if (claim != null) {
                    claim.close();
                }
                return ZLinkOneWayCalls.BACKPRESSURED;
            }
            return ZLinkOneWayCalls.SUBMITTED;
        } catch (RuntimeException failure) {
            if (payload != null) {
                payload.close();
            }
            if (lease != null) {
                lease.close();
            }
            if (claim != null) {
                claim.close();
            }
            throw failure;
        }
    }

    private void dispatchSend(
        ZLinkMeshDispatchRecord record,
        Namespace namespace,
        String packetName,
        Message payload,
        Map<String, String> metadata,
        String contentType,
        ZLinkMeshDrainCoordinator.Claim claim,
        ZLinkInboundDispatchBudget.Lease lease) {
        ChannelRouteSendHandlerRegistration route = namespace.routeSends.get(packetName);
        ChannelSendHandlerRegistration channel = namespace.channelSends.get(packetName);
        if (route == null && channel == null) {
            closeRecord(record, claim, lease);
            return;
        }
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            record,
            packetName);
        try {
            CompletionStage<Void> queued = namespace.sendQueue
                .enqueueWithPayloadBytes(payload.size(), () -> {
                lease.handlerStarted();
                CompletionStage<Void> invocation = route != null
                    ? invoker.executeHandler(() -> invoker.invokeRouteSendHandler(
                        null,
                                route,
                                record.receive().sourceNodeRid(),
                                payload,
                                metadata,
                                contentType))
                    : invoker.executeHandler(() ->
                        invoker.invokeSendHandler(
                            record.receive().channelName(),
                            channel,
                            payload,
                            metadata,
                            contentType));
                return invocation.whenComplete((ignored, error) -> {
                    if (error == null) {
                        traceFlow(
                            ZLinkMessageFlowOutcome.DISPATCHED,
                            record,
                            packetName);
                    }
                    closeRecord(record, claim, lease);
                });
            });
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    closeRecord(record, claim, lease);
                }
            });
        } catch (RuntimeException error) {
            closeRecord(record, claim, lease);
            throw error;
        }
    }

    private void dispatchRequest(
        ZLinkMeshDispatchRecord record,
        Namespace namespace,
        String packetName,
        Message payload,
        Map<String, String> metadata,
        String contentType,
        ZLinkMeshDrainCoordinator.Claim claim,
        ZLinkInboundDispatchBudget.Lease lease) {
        ReplyToken token = record.receive().replyToken();
        ChannelRouteRequestHandlerRegistration route = namespace.routeRequests.get(packetName);
        ChannelRequestHandlerRegistration channel = namespace.channelRequests.get(packetName);
        if ((token == null && !record.canReply())
            || (route == null && channel == null)) {
            reject(record, "MeshNode request handler is not registered: " + packetName, claim, lease);
            return;
        }
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            record,
            packetName);
        try {
            CompletionStage<Void> queued = namespace.requestQueue
                .enqueueWithPayloadBytes(payload.size(), () -> {
                return inboundDispatchBudget.acquireCompletionPermit()
                    .thenCompose(permit -> {
                        try {
                            lease.handlerStarted();
                            CompletionStage<Message> invocation = route != null
                                ? invoker.executeHandler(() -> invoker.invokeRouteRequestHandler(
                                    null,
                                    route,
                                    record.receive().sourceNodeRid(),
                                    payload,
                                    metadata,
                                    contentType))
                                : invoker.executeHandler(() ->
                                    invoker.invokeRequestHandler(
                                        record.receive().channelName(),
                                        channel,
                                        payload,
                                        metadata,
                                        contentType));
                            return invocation.<Void>handle((reply, error) -> {
                                try {
                                    if (error == null) {
                                        replyAndClose(record, token, List.of(reply));
                                        traceFlow(
                                            ZLinkMessageFlowOutcome.REPLIED,
                                            record,
                                            packetName);
                                    } else {
                                        replyError(record, token, error);
                                    }
                                } finally {
                                    permit.close();
                                }
                                return null;
                            });
                        } catch (RuntimeException failure) {
                            permit.close();
                            return java.util.concurrent.CompletableFuture
                                .<Void>failedFuture(failure);
                        }
                    })
                    .whenComplete((ignored, error) ->
                    closeRecord(record, claim, lease));
            });
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    closeRecord(record, claim, lease);
                }
            });
        } catch (RuntimeException error) {
            closeRecord(record, claim, lease);
            throw error;
        }
    }

    private void reject(ZLinkMeshDispatchRecord record, String message) {
        reject(record, message, null);
    }

    private void reject(
        ZLinkMeshDispatchRecord record,
        String message,
        ZLinkMeshDrainCoordinator.Claim claim) {
        reject(record, message, claim, null);
    }

    private void reject(
        ZLinkMeshDispatchRecord record,
        String message,
        ZLinkMeshDrainCoordinator.Claim claim,
        ZLinkInboundDispatchBudget.Lease lease) {
        ReplyToken token = record.receive().replyToken();
        try {
            if (token != null || record.canReply()) {
                replyError(record, token, message);
            }
        } finally {
            closeRecord(record, claim, lease);
        }
    }

    private static void closeRecord(
        ZLinkMeshDispatchRecord record,
        ZLinkMeshDrainCoordinator.Claim claim) {
        closeRecord(record, claim, null);
    }

    private static void closeRecord(
        ZLinkMeshDispatchRecord record,
        ZLinkMeshDrainCoordinator.Claim claim,
        ZLinkInboundDispatchBudget.Lease lease) {
        try {
            record.close();
        } finally {
            if (lease != null) {
                lease.close();
            }
            if (claim != null) {
                claim.close();
            }
        }
    }

    private void replyError(ReplyToken token, String message) {
        replyAndClose(token, ZLinkFrameworkErrorReply.create(message));
    }

    private void replyError(
        ZLinkMeshDispatchRecord record,
        ReplyToken token,
        String message) {
        replyAndClose(record, token, ZLinkFrameworkErrorReply.create(message));
    }

    private void replyError(
        ZLinkMeshDispatchRecord record,
        ReplyToken token,
        Throwable error) {
        Throwable cause = error;
        while ((cause instanceof java.util.concurrent.CompletionException
            || cause instanceof java.util.concurrent.ExecutionException)
            && cause.getCause() != null) {
            cause = cause.getCause();
        }
        systems.zlink.framework.errors.ZLinkFrameworkErrorKind kind =
            ZLinkChannelDispatchReporter.frameworkErrorKind(cause);
        replyAndClose(
            record,
            token,
            ZLinkFrameworkErrorReply.create(kind, cause.getMessage()));
    }

    private void replyAndClose(ReplyToken token, List<Message> parts) {
        try {
            replies.send(token, parts);
        } finally {
            parts.forEach(Message::close);
        }
    }

    private void replyAndClose(
        ZLinkMeshDispatchRecord record,
        ReplyToken token,
        List<Message> parts) {
        try {
            if (record.canReply()) {
                record.reply(parts);
            } else {
                replies.send(token, parts);
            }
        } finally {
            parts.forEach(Message::close);
        }
    }

    private Namespace namespace(RecordKind kind, String channelName) {
        return switch (kind) {
            case NODE_SEND, NODE_REQUEST -> namespaces.get(NODE_NAMESPACE);
            case CHANNEL_SEND, CHANNEL_REQUEST -> namespaces.get(channelName);
            default -> null;
        };
    }

    private void traceFlow(
        ZLinkMessageFlowOutcome outcome,
        ZLinkMeshDispatchRecord record,
        String packetName) {
        if (!flow.enabled(outcome)) {
            return;
        }
        ReceiveRecord receive = record.receive();
        ZLinkDispatchMessageKind kind = switch (receive.kind()) {
            case NODE_REQUEST, CHANNEL_REQUEST -> ZLinkDispatchMessageKind.REQUEST;
            default -> ZLinkDispatchMessageKind.SEND;
        };
        Long correlation = receive.applicationCorrelation();
        flow.trace(new ZLinkMessageFlowEvent(
            outcome,
            ZLinkDispatchErrorSurface.ROUTE_MESH_CHANNEL,
            kind,
            packetName,
            receive.channelName(),
            null,
            correlation == null ? null : Long.toUnsignedString(correlation),
            receive.sourceNodeRid() == null ? null : receive.sourceNodeRid().toString(),
            null,
            null,
            null));
    }

    private static Namespace routeNamespace(
        List<MeshNodeRegistration.DispatchHandler> handlers,
        int sendPendingCapacity,
        java.util.concurrent.Executor executor) {
        Namespace namespace = new Namespace(sendPendingCapacity, executor);
        for (MeshNodeRegistration.DispatchHandler handler : handlers) {
            String packetName = ZLinkPacketNames.resolve(handler.messageType());
            if (handler.request()) {
                putUnique(namespace.routeRequests, packetName,
                    new ChannelRouteRequestHandlerRegistration(
                        handler.handlerType(),
                        handler.messageType(),
                        handler.replyType(),
                        packetName));
            } else {
                putUnique(namespace.routeSends, packetName,
                    new ChannelRouteSendHandlerRegistration(
                        handler.handlerType(),
                        handler.messageType(),
                        packetName));
            }
        }
        return namespace;
    }

    @SuppressWarnings({"unchecked", "rawtypes"})
    private static Namespace channelNamespace(
        String channelName,
        List<MeshNodeRegistration.DispatchHandler> handlers,
        List<String> handlerGroups,
        ZLinkScannedHandlerCatalog scannedHandlers,
        java.util.concurrent.Executor executor) {
        Namespace namespace = new Namespace(Integer.MAX_VALUE, executor);
        Set<String> groups = handlerGroups.isEmpty()
            ? Set.of(channelName)
            : Set.copyOf(handlerGroups);
        for (ZLinkScannedHandler handler : scannedHandlers.matching(
            groups,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.REQUEST)) {
            putUnique(namespace.channelRequests, handler.packetName(),
                new ChannelRequestHandlerRegistration(
                    handler.handlerType(),
                    handler.handlerMethod(),
                    handler.messageType(),
                    handler.replyType(),
                    handler.packetName()));
        }
        for (ZLinkScannedHandler handler : scannedHandlers.matching(
            groups,
            ZLinkScannedHandlerSurface.CHANNEL,
            ZLinkScannedHandlerKind.SEND)) {
            putUnique(namespace.channelSends, handler.packetName(),
                new ChannelSendHandlerRegistration(
                    handler.handlerType(),
                    handler.handlerMethod(),
                    handler.messageType(),
                    handler.packetName()));
        }
        for (MeshNodeRegistration.DispatchHandler handler : handlers) {
            String packetName = ZLinkPacketNames.resolve(handler.messageType());
            if (handler.request()) {
                putUnique(namespace.channelRequests, packetName,
                    new ChannelRequestHandlerRegistration(
                        handler.handlerType(),
                        handler.messageType(),
                        handler.replyType(),
                        packetName));
            } else {
                putUnique(namespace.channelSends, packetName,
                    new ChannelSendHandlerRegistration(
                        handler.handlerType(),
                        handler.messageType(),
                        packetName));
            }
        }
        return namespace;
    }

    private static <T> void putUnique(Map<String, T> handlers, String packetName, T handler) {
        if (handlers.putIfAbsent(packetName, handler) != null) {
            throw new ZLinkConfigurationException(
                "duplicate MeshNode handler packet name: " + packetName);
        }
    }

    private static final class Namespace {
        private final Map<String, ChannelRouteSendHandlerRegistration> routeSends =
            new HashMap<>();
        private final Map<String, ChannelRouteRequestHandlerRegistration> routeRequests =
            new HashMap<>();
        private final Map<String, ChannelSendHandlerRegistration> channelSends =
            new HashMap<>();
        private final Map<String, ChannelRequestHandlerRegistration> channelRequests =
            new HashMap<>();
        private final ZLinkAsyncSerialQueue sendQueue;
        private final ZLinkAsyncSerialQueue requestQueue;

        Namespace(
            int sendPendingCapacity,
            java.util.concurrent.Executor executor) {
            sendQueue = new ZLinkAsyncSerialQueue(
                executor, false, sendPendingCapacity);
            requestQueue = new ZLinkAsyncSerialQueue(executor, false);
        }
    }
}
