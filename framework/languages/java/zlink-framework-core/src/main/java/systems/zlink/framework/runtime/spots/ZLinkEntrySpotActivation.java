package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime.LocalActorReply;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

final class EntrySpotActivation extends SpotActivationBase<DefaultEntrySpotContext> {
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
            ZLinkActor actor, ZLinkMessage createRequest, Object createContext) {
        ZLinkEntrySpot rawEntrySpot = entrySpot;
        if (createContext == context) {
            return ZLinkHandlerStages.fromStageSupplier(
                    () -> rawEntrySpot.onCreateActor(actor, createRequest));
        }
        CompletableFuture<ZLinkActorCreateResponse> response = new CompletableFuture<>();
        context.enqueueDispatch(
                        () ->
                                ZLinkHandlerStages.fromStageSupplier(
                                                () ->
                                                        (CompletionStage<ZLinkActorCreateResponse>)
                                                                rawEntrySpot.onCreateActor(
                                                                        actor, createRequest))
                                        .thenAccept(response::complete))
                .whenComplete(
                        (ignored, error) -> {
                            if (error != null) {
                                response.completeExceptionally(error);
                            }
                        });
        return response;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail, Supplier<CompletionStage<Void>> operation) {
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
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        return context.enqueueDispatch(payloadBytes, operation, admission);
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        if (host.isClosing()) {
            return CompletableFuture.completedFuture(null);
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            drainRoutes();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE) {
            drainSubscriptions();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
            var dispatched = dispatchActorMessages(info.actorMessages());
            dispatched.whenComplete(
                    (ignored, error) ->
                            info.actorMessages().forEach(ZLinkBackendActorReceived::close));
            return dispatched;
        }
        for (ZLinkBackendActorReceived actorMessage : info.actorMessages()) {
            actorMessage.close();
        }
        return CompletableFuture.completedFuture(null);
    }

    private void drainRoutes() {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return;
            }
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                ZLinkBackendReceived received =
                        backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    return;
                }
                batch.record(
                        ZLinkReceiveBatchBudget.bytesOf(
                                received.parts(),
                                received.applicationMetadataSize(),
                                received.acceptedJournalRecordSize()));
                if (host.dispatchSpotRouteBridgePacket(received)) {
                    received.close();
                    continue;
                }
                dispatchRoute(received);
            } finally {
                permit.abandonReservation();
            }
        }
    }

    CompletionStage<Void> admitRoute(ZLinkBackendReceived received) {
        return admitRoute(received, null);
    }

    CompletionStage<Void> admitRoute(
            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        CompletionStage<Void> dispatched =
                host.admitNewApplicationJob(
                        () -> {
                            var hostRejection = host.spotHostAdmissionFailure(context.spotId());
                            if (hostRejection != null)
                                return CompletableFuture.failedFuture(hostRejection);
                            if (host.dispatchSpotRouteBridgePacket(received)) {
                                received.close();
                                if (admission != null) admission.complete(null);
                                return CompletableFuture.completedFuture(null);
                            }
                            dispatchRoute(received, admission);
                            return CompletableFuture.completedFuture(null);
                        });
        dispatched.whenComplete(
                (done, failure) -> {
                    if (failure != null) {
                        received.close();
                        if (admission != null) admission.completeExceptionally(failure);
                    }
                });
        return dispatched;
    }

    void drainPolledDispatchQueues() {
        drainRoutes();
    }

    private void dispatchRoute(ZLinkBackendReceived received) {
        dispatchRoute(received, null);
    }

    private void dispatchRoute(ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        trackRouteReceived(received);
        //  Spec 27 §4: decode and install the inbound flow pair (or start a new
        //  flow) only while capture is enabled; at Off suppress flow state.
        ZLinkFlowContext.State inboundFlow = null;
        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header envelope = null;
        boolean captureFlow = host.flowCaptureEnabled();
        if (captureFlow) {
            try {
                envelope =
                        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope
                                .decodeDispatchHeader(received.parts(), true);
                inboundFlow =
                        envelope == null
                                ? ZLinkSpotFlowFrame.decode(received.parts())
                                : ZLinkSpotFlowFrame.fromEnvelopeHeader(envelope);
            } catch (ZLinkFrameworkException invalidFlow) {
                failRouteInvalidFlow(received, invalidFlow);
                if (admission != null) admission.complete(null);
                return;
            }
        }
        var flowScope =
                captureFlow
                        ? ZLinkFlowContext.enterOrCreate(inboundFlow, ZLinkFlowOrigin.INBOUND)
                        : ZLinkFlowContext.suppress();
        try {
            if (ZLinkSpotRuntime.isProbeFrame(received.parts())) {
                closeRouteReceived(received);
                if (admission != null) admission.complete(null);
                return;
            }
            ParsedPacket packet;
            try {
                packet =
                        envelope == null
                                ? ZLinkSpotRuntime.parsePacket(received.parts())
                                : ZLinkSpotRuntime.parsePacket(received.parts(), envelope);
            } catch (ZLinkFrameworkException invalidEnvelope) {
                //  A JSON-object first frame that is not a valid shared envelope
                //  is a protocol error (C++ decode parity).
                failRouteInvalidFlow(received, invalidEnvelope);
                if (admission != null) admission.complete(null);
                return;
            }
            host.traceSpotRouteFlow(
                    ZLinkMessageFlowOutcome.RECEIVED,
                    received.requestSeq().isPresent()
                            ? ZLinkDispatchMessageKind.REQUEST
                            : ZLinkDispatchMessageKind.SEND,
                    packet.packetName(),
                    received.requestSeq(),
                    backendSpot.spotId());
            if (ZLinkActorSpotRoutePackets.BOUND_SESSION_SEND_PACKET_NAME.equals(
                    packet.packetName())) {
                if (received.requestSeq().isPresent()) {
                    handleRoutedBoundSessionSendRequestParts(received.parts())
                            .thenAccept(received::reply)
                            .whenComplete((ignored, error) -> closeRouteReceived(received));
                } else {
                    handleRoutedBoundSessionSendParts(received.parts());
                    closeRouteReceived(received);
                }
                if (admission != null) admission.complete(null);
                return;
            }
            if (ZLinkActorSpotRoutePackets.ACTOR_PACKET_NAME.equals(packet.packetName())) {
                handleRoutedActorPacketParts(received.parts())
                        .thenAccept(
                                reply -> {
                                    if (received.requestSeq().isPresent()) {
                                        // A routed one-way Actor packet has no application
                                        // reply, but the route request still needs a
                                        // transport acknowledgement so the sender can retire
                                        // the handoff packet.
                                        received.reply(
                                                List.of(
                                                        reply.orElseGet(
                                                                ZLinkActorSpotRoutePackets
                                                                        ::createHandoffDirectReplyAck)));
                                    } else {
                                        reply.ifPresent(Message::close);
                                    }
                                })
                        .whenComplete((ignored, error) -> closeRouteReceived(received));
                if (admission != null) admission.complete(null);
                return;
            }
            dispatchSpotRouteHandler(received, packet, admission)
                    .whenComplete(
                            (ignored, error) -> {
                                if (admission != null && !admission.isDone()) {
                                    if (error == null) admission.complete(null);
                                    else admission.completeExceptionally(error);
                                }
                            });
        } finally {
            flowScope.close();
        }
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitCanonicalActorJoin(
            ZLinkActorSpotRoutePackets.TransferRequest request, RoutingId sourcePeerRid) {
        return host.actorAdmissions()
                .prepareCanonicalRoutedActor(
                        request,
                        null,
                        sourcePeerRid,
                        backendSpot.spotId(),
                        entrySpot,
                        null,
                        "application/octet-stream",
                        new byte[0],
                        actor -> host.notifySpotActorLifecycle(entrySpot, actor, true),
                        actorId ->
                                CompletableFuture.completedFuture(
                                        ZLinkSpotActorJoinResult.accept()));
    }

    private void drainSubscriptions() {
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return;
            }
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                ZLinkBackendTopicMessage received =
                        backendSpot.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    return;
                }
                batch.record(
                        ZLinkReceiveBatchBudget.bytesOf(
                                received.parts(),
                                received.applicationMetadataSize(),
                                received.topic().getBytes(StandardCharsets.UTF_8).length));
                dispatchSpotSubscription(received);
            } finally {
                permit.abandonReservation();
            }
        }
    }

    @Override
    CompletionStage<Void> dispatchResolvedActorPacket(
            ZLinkActor actor, ActorPacketFrames.Header packetHeader, ActorMessageRead read) {
        Object actorSpotSurface = host.localActorSpotSurface(actor);
        ZLinkActorSessionCoordinator.ActorRoute route =
                host.actorSessions()
                        .routeFor(
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
        ZLinkBackendActorReceived headerCopy =
                read.fromPendingHeader()
                        ? read.headerPart()
                        : ZLinkSpotRuntime.copyActorReceived(read.headerPart());
        Message payloadCopy =
                read.bodyPart() == null
                        ? Message.from(new byte[0])
                        : Message.from(read.bodyPart().message());
        return context.enqueueDispatch(
                () ->
                        dispatchRemoteJoinedActorPacket(
                                actor, route.actorRef(), packetHeader, headerCopy, payloadCopy));
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
            return CompletableFuture.failedFuture(
                    new ZLinkConfigurationException(
                            "remote joined actor packet is missing source session route: "
                                    + actor.context().actorId()));
        }
        ZLinkStreamHeader header =
                new ZLinkStreamHeader(
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
                    .thenAccept(
                            reply -> {
                                if (reply.isEmpty()) {
                                    return;
                                }
                                LocalActorReply actorReply = reply.get();
                                try (Message replyPayload = actorReply.payload();
                                        Message frame =
                                                ActorPacketFrames.encodeReply(
                                                        packetHeader,
                                                        replyPayload,
                                                        packetHeader.packetName(),
                                                        actorReply.codec())) {
                                    host.primaryNode()
                                            .replyActorNoBind(
                                                    headerPart.actor(),
                                                    headerPart.sourceNodeRid(),
                                                    headerPart.sourceSessionRid(),
                                                    headerPart.requestId(),
                                                    headerPart.flags(),
                                                    List.of(frame));
                                }
                            })
                    .whenComplete(
                            (ignored, error) -> {
                                payload.close();
                                headerPart.close();
                            });
        }
        try (Message headerPartMessage = Message.from(ZLinkStreamHeaderCodec.encode(header));
                Message body = Message.from(payload)) {
            boolean forwarded =
                    host.primaryNode()
                            .forwardActorBoundSession(
                                    targetActor,
                                    headerPart.sourceNodeRid(),
                                    headerPart.sourceSessionRid(),
                                    List.of(headerPartMessage, body),
                                    SendFlags.NONE);
            if (!forwarded) {
                return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                                "remote joined actor packet forward failed: "
                                        + actor.context().actorId()));
            }
            return CompletableFuture.completedFuture(null);
        } finally {
            payload.close();
            headerPart.close();
        }
    }

    CompletionStage<Void> closeAsync(Instant deadline) {
        return closingStage(deadline)
                .handle(
                        (ignored, failure) -> {
                            closePendingActorMessage();
                            closeActiveRouteReceives();
                            return finishCleanup(failure, context.closeResourcesAsync());
                        })
                .thenCompose(stage -> stage);
    }

    CompletionStage<Void> closingStage(Instant deadline) {
        return closingCallback(
                () ->
                        context.enqueueLifecycle(
                                () ->
                                        host.runWithOutbound(
                                                context.dispatchOutbound(),
                                                () ->
                                                        ZLinkHandlerStages.fromStageSupplier(
                                                                () ->
                                                                        entrySpot.onClosing(
                                                                                new systems.zlink
                                                                                        .framework
                                                                                        .spots
                                                                                        .ZLinkSpotClosingContext(
                                                                                        systems
                                                                                                .zlink
                                                                                                .framework
                                                                                                .spots
                                                                                                .ZLinkSpotCloseReason
                                                                                                .HOST_SHUTDOWN,
                                                                                        deadline))))));
    }
}
