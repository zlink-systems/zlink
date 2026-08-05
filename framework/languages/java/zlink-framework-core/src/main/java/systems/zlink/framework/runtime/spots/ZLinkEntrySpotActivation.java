package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;

import java.lang.reflect.Method;
import java.lang.reflect.InvocationTargetException;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CompletionException;
import java.util.concurrent.Executor;
import java.util.concurrent.Executors;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.function.Supplier;
import java.util.logging.Logger;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.contracts.errors.ZlinkSubmitException;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.contracts.sockets.SubmitResult;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.channels.ZLinkPublishCall;
import systems.zlink.framework.channels.ZLinkRequestCall;
import systems.zlink.framework.channels.ZLinkSendCall;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkDispatchErrorAction;
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.configuration.ZLinkDispatchFailure;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.channels.ChannelRegistration;
import systems.zlink.framework.runtime.channels.ChannelKind;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkDispatchErrorReporter;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerMethodInvoker;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerScanner;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandler;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerKind;
import systems.zlink.framework.runtime.handlers.ZLinkScannedHandlerCatalog;
import systems.zlink.framework.runtime.internal.handlers.ZLinkSuspendInvocationAdapter;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.messaging.ZLinkPayloadEncoding;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkPacketNames;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkWorkerTask;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotInfo;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotPublisherClient;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class EntrySpotActivation
    extends SpotActivationBase<DefaultEntrySpotContext> {
    private final ZLinkEntrySpot<?> entrySpot;

    EntrySpotActivation(
        ZLinkSpotRuntime host,
        ZLinkSpotHandlerInvoker spotHandlerInvoker,
        ZLinkEntrySpot<?> entrySpot,
        ZLinkBackendSpot backendSpot,
        DefaultEntrySpotContext context) {
        super(host, spotHandlerInvoker, entrySpot, backendSpot, context);
        this.entrySpot = entrySpot;
    }

    ZLinkEntrySpot<?> entrySpot() {
        return entrySpot;
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<ZLinkActorCreateResponse> notifyActorCreated(
        ZLinkActor actor,
        ZLinkMessage createRequest,
        Object createContext) {
        ZLinkEntrySpot rawEntrySpot = entrySpot;
        if (createContext == context) {
            return ZLinkHandlerStages.fromStageSupplier(() ->
                rawEntrySpot.onCreateActor(
                    actor,
                    createRequest));
        }
        java.util.concurrent.CompletableFuture<ZLinkActorCreateResponse> response =
            new java.util.concurrent.CompletableFuture<>();
        context.enqueueDispatch(() -> ZLinkHandlerStages.fromStageSupplier(() ->
                (CompletionStage<ZLinkActorCreateResponse>)
                    rawEntrySpot.onCreateActor(actor, createRequest))
            .thenAccept(response::complete))
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    response.completeExceptionally(error);
                }
            });
        return response;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
        CompletionStage<Void> tail,
        Supplier<CompletionStage<Void>> operation) {
        return context.enqueueDispatch(operation);
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
        CompletionStage<Void> tail,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return context.enqueueDispatch(payloadBytes, operation);
    }

    @Override
    CompletionStage<Void> appendActorLifecycle(
        CompletionStage<Void> tail,
        ZLinkBackendActorLifecycleEvent event,
        ZLinkBackendActorRef actorRef,
        ZLinkActor actor) {
        if (host.isClosing()) {
            return tail;
        }
        Supplier<CompletionStage<Void>> transition = host.actorLifecycleTransition(
            entrySpot,
            event,
            actorRef,
            actor,
            context.spotId());
        return transition == null ? tail : context.enqueueDispatch(transition);
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        if (host.isClosing()) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            drainRoutes();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE) {
            drainSubscriptions();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE) {
            drainUnhandledActorJoins();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
            return dispatchActorMessages(info.actorMessages())
                .whenComplete((ignored, error) -> info.actorMessages().forEach(
                    ZLinkBackendActorReceived::close));
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE) {
            drainActorLifecycleEvents();
        }
        for (ZLinkBackendActorReceived actorMessage : info.actorMessages()) {
            actorMessage.close();
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }

    private void drainRoutes() {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            ZLinkBackendReceived received =
                backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
            if (received == null) {
                return;
            }
            batch.record(ZLinkReceiveBatchBudget.bytesOf(
                received.parts(),
                received.applicationMetadataSize(),
                received.acceptedJournalRecordSize()));
            ZLinkSpotRuntime.traceSpotRouteInbound("entry-recv", backendSpot, received);
            if (host.dispatchSpotRouteBridgePacket(received)) {
                received.close();
                continue;
            }
            dispatchRoute(received);
        }
    }

    void drainPolledDispatchQueues() {
        drainRoutes();
        drainUnhandledActorJoins();
        drainActorLifecycleEvents();
    }

    private void dispatchRoute(ZLinkBackendReceived received) {
        var incomingFlow = ZLinkSpotFlowFrame.decode(received.parts());
        var flowScope = incomingFlow == null ? null
            : systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.enter(incomingFlow);
        try {
        trackRouteReceived(received);
        if (ZLinkSpotRuntime.isProbeFrame(received.parts())) {
            closeRouteReceived(received);
            return;
        }
        ParsedPacket packet = ZLinkSpotRuntime.parsePacket(received.parts());
        ZLinkSpotRuntime.traceSpotRouteDispatch("entry-dispatch", backendSpot, received, packet);
        host.traceMessageFlow(
            ZLinkMessageFlowOutcome.RECEIVED,
            ZLinkDispatchErrorSurface.SPOT_ROUTE,
            received.requestSeq().isPresent()
                ? ZLinkDispatchMessageKind.REQUEST
                : ZLinkDispatchMessageKind.SEND,
            packet.packetName(),
            null,
            null,
            received.requestSeq().map(String::valueOf).orElse(null),
            null,
            backendSpot.spotId().toString(),
            null);
        if (ZLinkActorSpotRoutePackets.JOIN_SPOT_PACKET_NAME.equals(packet.packetName())) {
            @SuppressWarnings({"rawtypes", "unchecked"})
            ZLinkEntrySpot rawEntrySpot = entrySpot;
            ZLinkRoutedActorTransferHandler transfer = new ZLinkRoutedActorTransferHandler(
                host,
                host.primaryNode(),
                backendSpot.spotId(),
                entrySpot,
                (actorId, request) -> java.util.concurrent.CompletableFuture.completedFuture(
                    ZLinkSpotActorJoinResult.accept()),
                actor -> host.notifySpotActorLifecycleAndSuppressBackendEvent(
                    entrySpot, actor, backendSpot.spotId(), true));
            transfer.handle(received.parts())
                .thenAccept(received::reply)
                .whenComplete((ignored, error) -> {
                    if (error != null) {
                        host.replySpotRouteDispatchError(
                            received,
                            packet.packetName(),
                            backendSpot.spotId(),
                            ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                            error);
                    }
                    closeRouteReceived(received);
                });
            return;
        }
        if (ZLinkActorSpotRoutePackets.BOUND_SESSION_SEND_PACKET_NAME.equals(packet.packetName())) {
            if (received.requestSeq().isPresent()) {
                handleRoutedBoundSessionSendRequestParts(received.parts())
                    .thenAccept(received::reply)
                    .whenComplete((ignored, error) -> closeRouteReceived(received));
            } else {
                handleRoutedBoundSessionSendParts(received.parts());
                closeRouteReceived(received);
            }
            return;
        }
        if (host.isDraining()
            && ZLinkActorSpotRoutePackets.ACTOR_PACKET_NAME.equals(packet.packetName())) {
            if (received.requestSeq().isPresent()) {
                host.replySpotRouteDispatchError(
                    received,
                    packet.packetName(),
                    backendSpot.spotId(),
                    ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.REJECTED,
                        "Actor application admission is sealed"));
            }
            closeRouteReceived(received);
            return;
        }
        if (ZLinkActorSpotRoutePackets.ACTOR_PACKET_NAME.equals(packet.packetName())) {
            handleRoutedActorPacketParts(received.parts())
                .thenAccept(reply -> {
                    if (received.requestSeq().isPresent()) {
                        reply.ifPresent(message -> received.reply(List.of(message)));
                    } else {
                        reply.ifPresent(Message::close);
                    }
                })
                .whenComplete((ignored, error) -> closeRouteReceived(received));
            return;
        }
        if (host.isDraining()) {
            if (received.requestSeq().isPresent()) {
                host.replySpotRouteDispatchError(
                    received,
                    packet.packetName(),
                    backendSpot.spotId(),
                    ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                    new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.REJECTED,
                        "SPOT application admission is sealed"));
            }
            closeRouteReceived(received);
            return;
        }
        dispatchSpotRouteHandler(received, packet);
        } finally {
            if (flowScope != null) flowScope.close();
        }
    }

    CompletionStage<Message> handleInternalActorTransfer(
        RoutingId sourceRoutingId,
        Message envelope) {
        List<Message> parts = systems.zlink.framework.runtime.actors
            .ZLinkActorEntryTransferEnvelope.decode(envelope);
        @SuppressWarnings({"rawtypes", "unchecked"})
        ZLinkEntrySpot rawEntrySpot = entrySpot;
        ZLinkRoutedActorTransferHandler transfer = new ZLinkRoutedActorTransferHandler(
            host,
            host.primaryNode(),
            backendSpot.spotId(),
            entrySpot,
            (actorId, request) -> java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept()),
            actor -> host.notifySpotActorLifecycleAndSuppressBackendEvent(
                entrySpot, actor, backendSpot.spotId(), true));
        CompletableFuture<Message> result = new CompletableFuture<>();
        context.enqueueDispatch(() -> transfer.handle(parts, sourceRoutingId)
                .thenAccept(replies -> {
                    try {
                        result.complete(systems.zlink.framework.runtime.actors
                            .ZLinkActorEntryTransferEnvelope.encode(replies));
                    } finally {
                        replies.forEach(Message::close);
                    }
                }))
            .whenComplete((ignored, error) -> {
                parts.forEach(Message::close);
                if (error != null) {
                    result.completeExceptionally(error);
                }
            });
        return result;
    }

    private void drainSubscriptions() {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            ZLinkBackendTopicMessage received =
                backendSpot.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
            if (received == null) {
                return;
            }
            batch.record(ZLinkReceiveBatchBudget.bytesOf(
                received.parts(),
                received.applicationMetadataSize(),
                received.topic().getBytes(StandardCharsets.UTF_8).length));
            dispatchSpotSubscription(received);
        }
    }

    @Override
    CompletionStage<Void> dispatchResolvedActorPacket(
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ActorMessageRead read) {
        Object actorSpotSurface = host.localActorSpotSurface(actor);
        ZLinkActorSessionCoordinator.ActorRoute route = host.actorSessions().routeFor(
            actor,
            host.primaryNode().routingId(),
            spotId -> host.spotSurfaceFor(spotId) != null);
        if (!route.remoteJoinedSpot()) {
            return host.dispatchLocalActorPacket(
                context,
                actorSpotSurface,
                actor,
                packetHeader,
                read.headerPart(),
                read.bodyPart(),
                read.fromPendingHeader());
        }
        ZLinkBackendActorReceived headerCopy = read.fromPendingHeader()
            ? read.headerPart()
            : ZLinkSpotRuntime.copyActorReceived(read.headerPart());
        Message payloadCopy = read.bodyPart() == null
            ? Message.from(new byte[0])
            : Message.from(read.bodyPart().message());
        return context.enqueueDispatch(() -> dispatchRemoteJoinedActorPacket(
            actor,
            route.actorRef(),
            packetHeader,
            headerCopy,
            payloadCopy));
    }

    private CompletionStage<Void> dispatchRemoteJoinedActorPacket(
        ZLinkActor actor,
        ZLinkBackendActorRef targetActor,
        ActorPacketFrames.Header packetHeader,
        ZLinkBackendActorReceived headerPart,
        Message payload) {
        if (headerPart.sourceNodeRid() == null || headerPart.sourceSessionRid() == null) {
            payload.close();
            headerPart.close();
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "remote joined actor packet is missing source session route: "
                    + actor.context().actorId()));
        }
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            packetHeader.requestSeq().isPresent()
                ? ZLinkStreamMessageKind.REQUEST
                : ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.fromValue(packetHeader.codec()),
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            packetHeader.requestSeq(),
            packetHeader.packetName(),
            Map.of());
        if (ZLinkSpotRuntime.isNoBindActorPacket(headerPart)
            || headerPart.sourceSessionRid().toBytes().length == 0) {
            return host.dispatchLocalSessionActor(targetActor, header, payload)
                .thenAccept(reply -> {
                    if (reply.isEmpty()) {
                        return;
                    }
                    try (Message replyPayload = reply.get();
                         Message frame = ActorPacketFrames.encodeReply(packetHeader, replyPayload)) {
                        host.primaryNode().replyActorNoBind(
                            headerPart.actor(),
                            headerPart.sourceNodeRid(),
                            headerPart.sourceSessionRid(),
                            headerPart.requestId(),
                            headerPart.flags(),
                            List.of(frame));
                    }
                })
                .whenComplete((ignored, error) -> {
                    payload.close();
                    headerPart.close();
                });
        }
        try (Message headerPartMessage = Message.from(ZLinkStreamHeaderCodec.encode(header));
             Message body = Message.from(payload)) {
            boolean forwarded = host.primaryNode().forwardActorBoundSession(
                targetActor,
                headerPart.sourceNodeRid(),
                headerPart.sourceSessionRid(),
                List.of(headerPartMessage, body),
                SendFlags.NONE);
            if (!forwarded) {
                return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                    "remote joined actor packet forward failed: "
                        + actor.context().actorId()));
            }
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        } finally {
            payload.close();
            headerPart.close();
        }
    }

    private void drainUnhandledActorJoins() {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            ZLinkBackendActorJoinRequest request =
                backendSpot.recvActorJoin(ZLinkBackendRecvMode.DONT_WAIT);
            if (request == null) {
                return;
            }
            batch.record(ZLinkReceiveBatchBudget.bytesOf(request.parts()));
            Message payloadCopy = request.parts().isEmpty()
                ? Message.from(new byte[0])
                : Message.from(request.parts().get(0).toByteArray());
            try {
                acceptEntryActorJoin(request, payloadCopy)
                    .whenComplete((response, error) -> {
                        try {
                            if (error != null) {
                                try (Message emptyReply = Message.from(new byte[0])) {
                                    backendSpot.replyActorJoin(request, 1, List.of(emptyReply));
                                }
                                return;
                            }
                            ZLinkSpotActorJoinResult effective =
                                response == null ? ZLinkSpotActorJoinResult.reject() : response;
                            Message reply = effective.reply() == null
                                ? Message.from(new byte[0])
                                : ZLinkMessagePayloads.message(effective.reply(), host.serializerForSpot());
                            backendSpot.replyActorJoin(request, effective.accepted() ? 0 : 1, List.of(reply));
                            reply.close();
                            if (effective.accepted()) {
                                completeAcceptedEntryJoin(request);
                            }
                        } finally {
                            payloadCopy.close();
                        }
                    });
            } finally {
                request.parts().forEach(Message::close);
            }
        }
    }

    private CompletionStage<ZLinkSpotActorJoinResult> acceptEntryActorJoin(
        ZLinkBackendActorJoinRequest request,
        Message payload) {
        return host.actorAdmissions().admitEntryActor(
            request,
            backendSpot.spotId(),
            actorId -> java.util.concurrent.CompletableFuture.completedFuture(
                ZLinkSpotActorJoinResult.accept()));
    }

    private void completeAcceptedEntryJoin(ZLinkBackendActorJoinRequest request) {
        host.actorAdmissions().completeEntryActorJoin(
            request,
            host.primaryNode().routingId(),
            actor -> context.enqueueDispatch(() ->
                host.notifySpotActorLifecycleAndSuppressBackendEvent(
                    entrySpot,
                    actor,
                    backendSpot.spotId(),
                    true)))
            .exceptionally(error -> null);
    }

    @Override
    public void close() {
        close(java.time.Instant.now());
    }

    void close(java.time.Instant deadline) {
        try {
            host.awaitClosing(context.enqueueDispatch(() ->
                host.runWithOutbound(context.dispatchOutbound(), () ->
                    ZLinkHandlerStages.fromStageSupplier(() ->
                        entrySpot.onClosing(
                            new systems.zlink.framework.spots
                                .ZLinkSpotClosingContext(
                                    systems.zlink.framework.spots
                                        .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                                    deadline))))));
        } finally {
            closePendingActorMessage();
            closeActiveRouteReceives();
            context.closeTimers();
            context.closeHandlerInstances();
            backendSpot.close();
        }
    }

}
