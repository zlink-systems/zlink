package systems.zlink.framework.runtime.channels;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.Executor;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;

import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Set;
import java.util.concurrent.CompletionStage;
import java.util.function.Consumer;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.binding.spot.Dispatch;
import systems.zlink.framework.runtime.internal.binding.spot.RecordKind;
import systems.zlink.framework.runtime.internal.binding.spot.ReceiveRecord;
import systems.zlink.framework.runtime.internal.binding.spot.ReplyToken;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshDispatchRecord;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshApplicationReceiver;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue;

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
    private final ZLinkApplicationJobQueue applicationJobQueue;
    private final ZLinkMessageFlowTracer flow;
    private final Map<String, Namespace> namespaces = new HashMap<>();

    public ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory) {
        this(mesh, serializer, framework, handlerFactory,
            (token, parts) -> Dispatch.reply(token, parts, SendFlags.NONE), null);
    }

    public ZLinkMeshApplicationDispatcher(
        MeshNodeRegistration mesh,
        ZLinkMessageSerializer serializer,
        ZLinkFrameworkRegistration framework,
        ZLinkHandlerActivator handlerFactory,
        ZLinkMeshDrainCoordinator drains) {
        this(mesh, serializer, framework, handlerFactory,
            (token, parts) -> Dispatch.reply(token, parts, SendFlags.NONE), drains);
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
        Objects.requireNonNull(mesh, "mesh");
        Objects.requireNonNull(framework, "framework");
        this.meshName = mesh.meshName();
        this.drains = drains;
        this.applicationJobQueue = framework.applicationJobQueue();
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
        namespaces.put(
            NODE_NAMESPACE,
            routeNamespace(
                mesh.nodeHandlers(),
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

        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope;
        try {
            envelope = systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader(record.parts(), false);
        } catch (systems.zlink.framework.errors.ZLinkFrameworkException invalidEnvelope) {
            //  A JSON-object first frame that is not a valid shared envelope
            //  is a protocol error (C++ decode parity).
            reject(record, invalidEnvelope.getMessage(), claim);
            return;
        }
        String packetName = envelope != null
            ? envelope.messageName()
            : record.parts().get(0).toUtf8String();
        Message payload = record.parts().get(1);
        String contentType = envelope != null
            ? envelope.contentType()
            : record.receive().contentType() != null
                ? record.receive().contentType()
                : ZLinkChannelContentTypeFrame.decode(record.parts());
        Map<String, String> metadata;
        try {
            metadata = envelope != null && !envelope.metadata().isEmpty()
                ? envelope.metadata()
                : ZLinkApplicationMetadata.decode(
                    record.receive().applicationMetadata());
        } catch (IllegalArgumentException error) {
            reject(record, error.getMessage(), claim);
            return;
        }
        switch (kind) {
            case NODE_SEND, CHANNEL_SEND ->
                  dispatchSend(
                      record, namespace, packetName, payload, metadata, contentType, claim);
            case NODE_REQUEST, CHANNEL_REQUEST ->
                  dispatchRequest(
                      record, namespace, envelope, packetName, payload, metadata,
                      contentType, claim);
            default -> closeRecord(record, claim);
        }
    }

    @Override
    public void setLocalNodeReadyHandler(Runnable handler) {
        Objects.requireNonNull(handler, "handler");
    }

    @Override
    public CompletionStage<Integer> submitLocalNodeSend(
        RoutingId sourceNodeRid,
        byte[] metadataBytes,
        List<Message> parts) {
        Namespace namespace = namespaces.get(NODE_NAMESPACE);
        if (namespace == null || parts.size() < 2) {
            return CompletableFuture.completedFuture(
                ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope;
        try {
            envelope = systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.decodeDispatchHeader(parts, false);
        } catch (systems.zlink.framework.errors.ZLinkFrameworkException invalidEnvelope) {
            return CompletableFuture.completedFuture(
                ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        String packetName = envelope != null
            ? envelope.messageName()
            : parts.get(0).toUtf8String();
        ChannelRouteSendHandlerRegistration route = namespace.routeSends.get(packetName);
        if (route == null) {
            return CompletableFuture.completedFuture(
                ZLinkOneWayCalls.TARGET_NOT_FOUND);
        }
        ZLinkMeshDrainCoordinator.Claim claim = drains == null
            ? null
            : drains.tryClaim(meshName);
        if (drains != null && claim == null) {
            return CompletableFuture.completedFuture(
                ZLinkOneWayCalls.SHUTDOWN);
        }

        Message payload = null;
        try {
            payload = Message.from(parts.get(1));
            Map<String, String> metadata =
                envelope != null && !envelope.metadata().isEmpty()
                    ? envelope.metadata()
                    : ZLinkApplicationMetadata.decode(metadataBytes);
            String contentType = envelope != null
                ? envelope.contentType()
                : ZLinkChannelContentTypeFrame.decode(parts);
            Message ownedPayload = payload;
            CompletableFuture<ZLinkApplicationJobQueue.Permit> acquisition =
                applicationJobQueue.acquire().toCompletableFuture();
            CompletableFuture<Integer> admission = new CompletableFuture<>();
            admission.whenComplete((ignored, failure) -> {
                if (admission.isCancelled()) {
                    acquisition.cancel(false);
                }
            });
            acquisition.whenComplete((permit, failure) -> {
                if (failure != null) {
                    closeLocalSubmission(ownedPayload, claim);
                    if (!admission.isCancelled()) {
                        admission.completeExceptionally(failure);
                    }
                    return;
                }
                if (admission.isCancelled()) {
                    permit.close();
                    closeLocalSubmission(ownedPayload, claim);
                    return;
                }
                try (ZLinkApplicationJobContext.Scope ignored =
                         ZLinkApplicationJobContext.enter(permit)) {
                    boolean accepted = namespace.sendQueue.tryEnqueue(() -> {
                        try {
                            traceLocalNodeSend(
                                ZLinkMessageFlowOutcome.ADMITTED,
                                packetName,
                                sourceNodeRid);
                            traceLocalNodeSend(
                                ZLinkMessageFlowOutcome.DISPATCHED,
                                packetName,
                                sourceNodeRid);
                            return invoker.executeHandler(() ->
                                    invoker.invokeRouteSendHandler(
                                        null,
                                        route,
                                        sourceNodeRid,
                                        ownedPayload,
                                        metadata,
                                        contentType))
                                .whenComplete((unused, error) -> {
                                    if (error == null) {
                                        traceLocalNodeSend(
                                            ZLinkMessageFlowOutcome.COMPLETED,
                                            packetName,
                                            sourceNodeRid);
                                    }
                                    closeLocalSubmission(
                                        ownedPayload, claim);
                                });
                        } catch (RuntimeException handlerFailure) {
                            closeLocalSubmission(ownedPayload, claim);
                            return CompletableFuture.failedFuture(
                                handlerFailure);
                        }
                    });
                    if (!accepted) {
                        traceLocalNodeSend(
                            ZLinkMessageFlowOutcome.BACKPRESSURED,
                            packetName,
                            sourceNodeRid);
                        permit.abandonReservation();
                        closeLocalSubmission(ownedPayload, claim);
                        admission.complete(ZLinkOneWayCalls.BACKPRESSURED);
                        return;
                    }
                    permit.abandonReservation();
                    admission.complete(ZLinkOneWayCalls.SUBMITTED);
                } catch (RuntimeException dispatchFailure) {
                    permit.abandonReservation();
                    closeLocalSubmission(ownedPayload, claim);
                    admission.completeExceptionally(dispatchFailure);
                }
            });
            return admission;
        } catch (RuntimeException failure) {
            if (payload != null) {
                payload.close();
            }
            if (claim != null) {
                claim.close();
            }
            throw failure;
        }
    }

    private static void closeLocalSubmission(
        Message payload,
        ZLinkMeshDrainCoordinator.Claim claim) {
        try {
            payload.close();
        } finally {
            if (claim != null) {
                claim.close();
            }
        }
    }

    private void dispatchSend(
        ZLinkMeshDispatchRecord record,
        Namespace namespace,
        String packetName,
        Message payload,
        Map<String, String> metadata,
        String contentType,
        ZLinkMeshDrainCoordinator.Claim claim) {
        ChannelRouteSendHandlerRegistration route = namespace.routeSends.get(packetName);
        ChannelSendHandlerRegistration channel = namespace.channelSends.get(packetName);
        if (route == null && channel == null) {
            closeRecord(record, claim);
            return;
        }
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            record,
            packetName);
        try {
            CompletionStage<Void> queued = namespace.sendQueue
                .enqueue(() -> {
                traceFlow(
                    ZLinkMessageFlowOutcome.ADMITTED,
                    record,
                    packetName);
                traceFlow(
                    ZLinkMessageFlowOutcome.DISPATCHED,
                    record,
                    packetName);
                CompletionStage<Void> invocation = route != null
                    ? invoker.executeHandler(() -> invoker.invokeRouteSendHandler(
                                record.receive().channelName(),
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
                            ZLinkMessageFlowOutcome.COMPLETED,
                            record,
                            packetName);
                    }
                    closeRecord(record, claim);
                });
            });
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    closeRecord(record, claim);
                }
            });
        } catch (RuntimeException error) {
            closeRecord(record, claim);
            throw error;
        }
    }

    private void dispatchRequest(
        ZLinkMeshDispatchRecord record,
        Namespace namespace,
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope,
        String packetName,
        Message payload,
        Map<String, String> metadata,
        String contentType,
        ZLinkMeshDrainCoordinator.Claim claim) {
        ReplyToken token = record.receive().replyToken();
        ChannelRouteRequestHandlerRegistration route = namespace.routeRequests.get(packetName);
        ChannelRequestHandlerRegistration channel = namespace.channelRequests.get(packetName);
        if ((token == null && !record.canReply())
            || (route == null && channel == null)) {
            // Mirrors the one-way TARGET_NOT_FOUND convention just above
            // (submitLocalNodeSend) and every other cross-language host's
            // route-mesh reply for an unregistered request handler: NOT_FOUND,
            // not the reject(...) overload's INTERNAL_FAILURE default.
            //  The error envelope must echo the request's channel/message
            //  names and correlation id exactly like the handler-failure
            //  path below (replyError(record, token, envelope, error)) and
            //  like every other language's reply writer. An error envelope
            //  built without the request header carries no messageName, and
            //  a peer that validates the reply header against the request
            //  cannot decode it as a framework error at all -- it surfaces
            //  as an unclassified internal failure instead of NOT_FOUND.
            reject(
                record,
                envelope,
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "MeshNode request handler is not registered: " + packetName,
                claim);
            return;
        }
        traceFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            record,
            packetName);
        try {
            CompletionStage<Void> queued = namespace.requestQueue
                .enqueue(() -> {
                traceFlow(
                    ZLinkMessageFlowOutcome.ADMITTED,
                    record,
                    packetName);
                traceFlow(
                    ZLinkMessageFlowOutcome.DISPATCHED,
                    record,
                    packetName);
                try {
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
                                if (error == null) {
                                    replyAndClose(record, token, replyParts(envelope, reply));
                                    traceFlow(
                                        ZLinkMessageFlowOutcome.REPLIED,
                                        record,
                                        packetName);
                                } else {
                                    replyError(record, token, envelope, error);
                                }
                                return null;
                            });
                } catch (RuntimeException failure) {
                    return CompletableFuture.<Void>failedFuture(failure);
                }
            }).whenComplete((ignored, error) ->
                closeRecord(record, claim));
            queued.whenComplete((ignored, error) -> {
                if (error != null) {
                    closeRecord(record, claim);
                }
            });
        } catch (RuntimeException error) {
            closeRecord(record, claim);
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
        ReplyToken token = record.receive().replyToken();
        try {
            if (token != null || record.canReply()) {
                replyError(record, token, message);
            }
        } finally {
            closeRecord(record, claim);
        }
    }

    private void reject(
        ZLinkMeshDispatchRecord record,
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope,
        ZLinkFrameworkErrorKind kind,
        String message,
        ZLinkMeshDrainCoordinator.Claim claim) {
        ReplyToken token = record.receive().replyToken();
        try {
            if (token != null || record.canReply()) {
                replyAndClose(
                    record,
                    token,
                    ZLinkFrameworkErrorReply.create(
                        envelope, kind, message, Map.of()));
            }
        } finally {
            closeRecord(record, claim);
        }
    }

    private static void closeRecord(
        ZLinkMeshDispatchRecord record,
        ZLinkMeshDrainCoordinator.Claim claim) {
        try {
            record.close();
        } finally {
            if (claim != null) {
                claim.close();
            }
        }
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
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope,
        Throwable error) {
        Throwable cause = error;
        while ((cause instanceof CompletionException
            || cause instanceof ExecutionException)
            && cause.getCause() != null) {
            cause = cause.getCause();
        }
        ZLinkFrameworkErrorKind kind =
            ZLinkChannelDispatchReporter.frameworkErrorKind(cause);
        replyAndClose(
            record,
            token,
            ZLinkFrameworkErrorReply.create(
                envelope, kind, cause.getMessage(), Map.of()));
    }

    /**
     * Successful request reply parts: a kind-2 envelope echoing the request
     * identifiers when the request arrived as a shared envelope, else the
     * legacy raw single-part reply.
     */
    private static List<Message> replyParts(
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope,
        Message reply) {
        if (envelope == null) {
            return List.of(reply);
        }
        return List.of(
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encodeHeader(systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.reply(envelope)),
            reply);
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
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(outcome);
        if (tracePoint == null) {
            return;
        }
        ReceiveRecord receive = record.receive();
        ZLinkDispatchMessageKind kind = switch (receive.kind()) {
            case NODE_REQUEST, CHANNEL_REQUEST -> ZLinkDispatchMessageKind.REQUEST;
            default -> ZLinkDispatchMessageKind.SEND;
        };
        Long correlation = receive.applicationCorrelation();
        tracePoint.trace(new ZLinkMessageFlowEvent(
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

    private void traceLocalNodeSend(
        ZLinkMessageFlowOutcome phase,
        String packetName,
        RoutingId sourceNodeRid) {
        ZLinkMessageFlowTracer.TracePoint tracePoint = flow.begin(phase);
        if (tracePoint == null) {
            return;
        }
        tracePoint.trace(new ZLinkMessageFlowEvent(
            phase,
            ZLinkDispatchErrorSurface.NODE,
            ZLinkDispatchMessageKind.SEND,
            packetName,
            null,
            null,
            null,
            sourceNodeRid == null ? null : sourceNodeRid.toString(),
            null,
            null,
            null));
    }

    private static Namespace routeNamespace(
        List<MeshNodeRegistration.DispatchHandler> handlers,
        Executor executor) {
        Namespace namespace = new Namespace(executor);
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
        Executor executor) {
        Namespace namespace = new Namespace(executor);
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
        for (ZLinkScannedHandler handler : scannedHandlers.matching(
            groups,
            ZLinkScannedHandlerSurface.ROUTE,
            ZLinkScannedHandlerKind.SEND)) {
            putUnique(namespace.routeSends, handler.packetName(),
                new ChannelRouteSendHandlerRegistration(
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
            } else if (handler.routeContext()) {
                putUnique(namespace.routeSends, packetName,
                    new ChannelRouteSendHandlerRegistration(
                        handler.handlerType(),
                        handler.messageType(),
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

        Namespace(Executor executor) {
            sendQueue = new ZLinkAsyncSerialQueue(
                executor, ZLinkExecutionLanePolicy.generic());
            requestQueue = new ZLinkAsyncSerialQueue(
                executor, ZLinkExecutionLanePolicy.generic());
        }
    }
}
