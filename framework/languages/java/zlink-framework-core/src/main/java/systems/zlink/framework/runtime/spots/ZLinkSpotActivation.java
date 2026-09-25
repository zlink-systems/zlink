package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotActorJoinResult;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotClosingContext;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

final class SpotActivation extends SpotActivationBase<DefaultSpotContext> {
    private final ZLinkSpot<?> spot;

    SpotActivation(
            ZLinkSpotRuntime host,
            ZLinkSpotHandlerInvoker spotHandlerInvoker,
            ZLinkSpot<?> spot,
            ZLinkBackendSpot backendSpot,
            DefaultSpotContext context) {
        super(host, spotHandlerInvoker, spot, backendSpot, context);
        this.spot = spot;
    }

    ZLinkSpot<?> spot() {
        return spot;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail, Supplier<CompletionStage<Void>> operation) {
        return tail.thenCompose(ignored -> operation.get());
    }

    @Override
    CompletionStage<Void> appendActorLifecycle(
            CompletionStage<Void> tail,
            ZLinkBackendActorLifecycleEvent event,
            ZLinkBackendActorRef actorRef,
            ZLinkActor actor) {
        if (host.shouldRunActorLifecycleInSpotDispatch(event, actor)) {
            return tail.thenCompose(
                    ignored ->
                            context.enqueueLifecycle(
                                    () -> {
                                        if (host.isClosing()) {
                                            return CompletableFuture.completedFuture(null);
                                        }
                                        Supplier<CompletionStage<Void>> transition =
                                                host.actorLifecycleTransition(
                                                        spot,
                                                        event,
                                                        actorRef,
                                                        actor,
                                                        context.spotId());
                                        return transition == null
                                                ? CompletableFuture.completedFuture(null)
                                                : beginApplicationLifecycle(transition);
                                    }));
        }
        return host.actorSessions()
                .dispatch(
                        actor,
                        () ->
                                context.enqueueActorDispatch(
                                        actor.context().actorId(),
                                        () -> {
                                            if (host.isClosing()) {
                                                return CompletableFuture.completedFuture(null);
                                            }
                                            Supplier<CompletionStage<Void>> transition =
                                                    host.actorLifecycleTransition(
                                                            spot,
                                                            event,
                                                            actorRef,
                                                            actor,
                                                            context.spotId());
                                            if (transition == null) {
                                                return CompletableFuture.completedFuture(null);
                                            }
                                            return beginApplicationLifecycle(transition);
                                        }));
    }

    private static CompletionStage<Void> beginApplicationLifecycle(
            Supplier<CompletionStage<Void>> transition) {
        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                .beforeFirstApplicationInstruction();
        return transition.get();
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        if (host.isClosing()) {
            return CompletableFuture.completedFuture(null);
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            return drainRoutesForDispatch();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE) {
            return drainActorLifecycleEvents();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE) {
            return drainUnhandledActorJoinsAsync();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE
                && host.isActorInfrastructureControl(info.actorMessages())) {
            CompletionStage<Void> control =
                    context.enqueueInfrastructureDispatch(
                            () -> dispatchActorMessages(info.actorMessages()));
            return control.whenComplete(
                    (ignored, error) -> {
                        for (ZLinkBackendActorReceived actorMessage : info.actorMessages()) {
                            actorMessage.close();
                        }
                    });
        }
        return context.enqueueDispatch(
                () ->
                        dispatchEventAsync(info)
                                .whenComplete(
                                        (ignored, error) -> {
                                            for (ZLinkBackendActorReceived actorMessage :
                                                    info.actorMessages()) {
                                                actorMessage.close();
                                            }
                                        }));
    }

    private CompletionStage<Void> dispatchEventAsync(ZLinkBackendSpotDispatchInfo info) {
        if (info.event() == ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            return drainRoutesAsync();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.SUBSCRIBE_READABLE) {
            return drainSubscriptionsAsync();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_JOIN_READABLE) {
            return drainUnhandledActorJoinsAsync();
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
            return dispatchActorMessages(info.actorMessages());
        }
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_LIFECYCLE_READABLE) {
            return drainActorLifecycleEvents();
        }
        return CompletableFuture.completedFuture(null);
    }

    private CompletionStage<Void> drainRoutesForDispatch() {
        List<CompletableFuture<Void>> completions = new ArrayList<>();
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                break;
            }
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                ZLinkBackendReceived received =
                        backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    break;
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
                var replyRoute =
                        host.registerRelocationReplyLazy(
                                () -> ZLinkSpotAcceptedJournal.encode(received),
                                received,
                                context.spotId(),
                                backendSpot.lifecycleGeneration());
                completions.add(
                        context.enqueueAcceptedDispatch(
                                        replyRoute::record,
                                        received.acceptedJournalRecordSize(),
                                        () ->
                                                dispatchRouteAsync(received)
                                                        .whenComplete(
                                                                (ignoredResult, failure) ->
                                                                        replyRoute.completeLocal()),
                                        replyRoute::releaseForRelocation)
                                .toCompletableFuture());
            } finally {
                permit.abandonReservation();
            }
        }
        return CompletableFuture.allOf(completions.toArray(CompletableFuture[]::new));
    }

    private CompletionStage<Void> dispatchRoutesAsync(List<ZLinkBackendReceived> routes) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (ZLinkBackendReceived received : routes) {
            tail = tail.thenCompose(ignored -> dispatchRouteAsync(received));
        }
        return tail;
    }

    void drainPolledDispatchQueues() {
        drainUnhandledActorJoinsAsync().exceptionally(error -> null);
        drainRoutesForDispatch();
        drainActorLifecycleEvents();
    }

    private CompletionStage<Void> drainRoutesAsync() {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return tail;
            }
            systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .QueuedOwnership
                    ownership = null;
            ZLinkBackendReceived received;
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                received = backendSpot.recvRoute(ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    return tail;
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
                ownership =
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                                .transferToQueuedJob();
            } finally {
                permit.abandonReservation();
            }
            CompletionStage<Void> prior = tail;
            var queuedOwnership = ownership;
            tail =
                    prior.thenCompose(
                            ignored ->
                                    host.runQueuedApplicationJob(
                                            queuedOwnership, () -> dispatchRouteAsync(received)));
        }
        return tail;
    }

    private CompletionStage<Void> dispatchRouteAsync(ZLinkBackendReceived received) {
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
                return CompletableFuture.completedFuture(null);
            }
        }
        var flowScope =
                captureFlow
                        ? ZLinkFlowContext.enterOrCreate(inboundFlow, ZLinkFlowOrigin.INBOUND)
                        : ZLinkFlowContext.suppress();
        try {
            if (ZLinkSpotRuntime.isProbeFrame(received.parts())) {
                closeRouteReceived(received);
                return CompletableFuture.completedFuture(null);
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
                return CompletableFuture.completedFuture(null);
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
                CompletionStage<?> stage =
                        received.requestSeq().isPresent()
                                ? handleRoutedBoundSessionSendRequestParts(received.parts())
                                        .thenAccept(received::reply)
                                : handleRoutedBoundSessionSendParts(received.parts());
                return stage.thenApply(ignored -> (Void) null)
                        .whenComplete((ignored, error) -> closeRouteReceived(received));
            }
            if (host.isDraining() || closeCommitted()) {
                if (received.requestSeq().isPresent()) {
                    host.replySpotRouteDispatchError(
                            received,
                            packet.packetName(),
                            backendSpot.spotId(),
                            ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                            //  Sealed admission is a framework-generated rejection, so
                            //  the reply carries the framework-origin marker.
                            host.spotAdmissionFailure(backendSpot.spotId()));
                }
                closeRouteReceived(received);
                return CompletableFuture.completedFuture(null);
            }
            if (ZLinkActorSpotRoutePackets.ACTOR_PACKET_NAME.equals(packet.packetName())) {
                return handleRoutedActorPacketParts(received.parts())
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
                        .thenApply(ignored -> (Void) null)
                        .whenComplete((ignored, error) -> closeRouteReceived(received));
            }
            return dispatchSpotRouteHandler(received, packet);
        } finally {
            flowScope.close();
        }
    }

    CompletionStage<List<byte[]>> replayAccepted(ZLinkSpotAcceptedJournal.Record record) {
        Objects.requireNonNull(record, "record");
        CompletableFuture<List<byte[]>> reply = new CompletableFuture<>();
        List<Message> parts = record.parts().stream().map(Message::from).toList();
        var received =
                new ZLinkBackendReceived(
                        record.result(),
                        record.routingId(),
                        record.spotId(),
                        record.requestSequence(),
                        record.applicationMetadata(),
                        new byte[0],
                        parts,
                        values -> {
                            try {
                                reply.complete(values.stream().map(Message::toByteArray).toList());
                            } finally {
                                values.forEach(Message::close);
                            }
                        },
                        () -> {});
        CompletionStage<Void> dispatched;
        try {
            dispatched = dispatchRouteAsync(received);
        } catch (RuntimeException failure) {
            received.close();
            return CompletableFuture.failedFuture(failure);
        }
        return dispatched.thenCompose(
                ignored ->
                        record.requestSequence().isPresent()
                                ? reply
                                : CompletableFuture.completedFuture(List.of()));
    }

    private CompletionStage<Void> drainSubscriptionsAsync() {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return tail;
            }
            systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .QueuedOwnership
                    ownership;
            ZLinkBackendTopicMessage received;
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                received = backendSpot.subscribe(ZLinkBackendRecvMode.DONT_WAIT);
                if (received == null) {
                    return tail;
                }
                batch.record(
                        ZLinkReceiveBatchBudget.bytesOf(
                                received.parts(),
                                received.applicationMetadataSize(),
                                received.topic().getBytes(StandardCharsets.UTF_8).length));
                ownership =
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                                .transferToQueuedJob();
            } finally {
                permit.abandonReservation();
            }
            CompletionStage<Void> prior = tail;
            var queuedOwnership = ownership;
            tail =
                    prior.thenCompose(
                            ignored ->
                                    host.runQueuedApplicationJob(
                                            queuedOwnership,
                                            () -> dispatchSpotSubscription(received)));
        }
        return tail;
    }

    private CompletionStage<Void> drainUnhandledActorJoinsAsync() {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return tail;
            }
            systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .QueuedOwnership
                    ownership;
            ZLinkBackendActorJoinRequest request;
            try (var ignored =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .enter(permit)) {
                request = backendSpot.recvActorJoin(ZLinkBackendRecvMode.DONT_WAIT);
                if (request == null) {
                    return tail;
                }
                batch.record(ZLinkReceiveBatchBudget.bytesOf(request.parts()));
                ownership =
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                                .transferToQueuedJob();
            } finally {
                permit.abandonReservation();
            }
            CompletionStage<Void> prior = tail;
            var queuedOwnership = ownership;
            tail =
                    prior.thenCompose(
                            ignored ->
                                    host.runQueuedApplicationJob(
                                            queuedOwnership,
                                            () -> dispatchActorJoinAsync(request)));
        }
        return tail;
    }

    private CompletionStage<Void> dispatchActorJoinAsync(ZLinkBackendActorJoinRequest request) {
        Message payloadCopy = actorJoinPayload(request.parts());
        request.parts().forEach(Message::close);
        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                .beforeFirstApplicationInstruction();
        return host.runWithOutbound(
                        context.dispatchOutbound(),
                        () -> invokeActorJoinCallback(request, payloadCopy))
                .handle(
                        (response, error) -> {
                            if (error != null) {
                                try (Message emptyReply = Message.from(new byte[0])) {
                                    backendSpot.replyActorJoin(request, 1, List.of(emptyReply));
                                }
                                return null;
                            }
                            ZLinkSpotActorJoinResult effective =
                                    response == null ? ZLinkSpotActorJoinResult.reject() : response;
                            Message reply =
                                    effective.reply() == null
                                            ? Message.from(new byte[0])
                                            : ZLinkMessagePayloads.message(
                                                    effective.reply(), host.serializerForSpot());
                            try {
                                backendSpot.replyActorJoin(
                                        request, effective.accepted() ? 0 : 1, List.of(reply));
                            } finally {
                                reply.close();
                            }
                            return null;
                        })
                .thenApply(ignored -> (Void) null)
                .whenComplete((ignored, error) -> payloadCopy.close());
    }

    private Message actorJoinPayload(List<Message> parts) {
        if (parts.isEmpty()) {
            return Message.from(new byte[0]);
        }
        return Message.from(parts.get(0).dataBuffer());
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    CompletionStage<ZLinkSpotActorJoinResult> admitCanonicalActorJoin(
            ZLinkActorSpotRoutePackets.TransferRequest request,
            RoutingId sourcePeerRid,
            Message payload,
            systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec.ActorJoin28
                    canonicalJoin,
            String requestContentType) {
        return host.actorAdmissions()
                .prepareCanonicalRoutedActor(
                        request,
                        null,
                        sourcePeerRid,
                        backendSpot.spotId(),
                        host.spotFor(backendSpot.spotId()),
                        canonicalJoin,
                        requestContentType,
                        payload.toByteArray(),
                        actor ->
                                host.notifySpotActorLifecycleAndSuppressBackendEvent(
                                        spot, actor, backendSpot.spotId(), true),
                        actorId ->
                                host.runWithOutbound(
                                        context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromStageSupplier(
                                                        () ->
                                                                (CompletionStage<
                                                                                ZLinkSpotActorJoinResult>)
                                                                        ((ZLinkSpot) spot)
                                                                                .onActorJoin(
                                                                                        actorId,
                                                                                        ZLinkMessage
                                                                                                .fromEncoded(
                                                                                                        ZLinkMessagePayloads
                                                                                                                .encoded(
                                                                                                                        payload),
                                                                                                        host
                                                                                                                .serializerForSpot())))));
    }

    @SuppressWarnings({"rawtypes", "unchecked"})
    private CompletionStage<ZLinkSpotActorJoinResult> invokeActorJoinCallback(
            ZLinkBackendActorJoinRequest request, Message payload) {
        return host.actorAdmissions()
                .admitSpotActor(
                        request,
                        backendSpot.spotId(),
                        host.spotFor(backendSpot.spotId()),
                        actorId ->
                                ZLinkHandlerStages.fromStageSupplier(
                                        () ->
                                                (CompletionStage<ZLinkSpotActorJoinResult>)
                                                        ((ZLinkSpot) spot)
                                                                .onActorJoin(
                                                                        actorId,
                                                                        ZLinkMessage.fromEncoded(
                                                                                ZLinkMessagePayloads
                                                                                        .encoded(
                                                                                                payload),
                                                                                host
                                                                                        .serializerForSpot()))),
                        actor ->
                                host.notifySpotActorLifecycleAndSuppressBackendEvent(
                                        spot, actor, backendSpot.spotId(), true));
    }

    @Override
    public void close() {
        close(ZLinkSpotCloseReason.EXPLICIT_CLOSE, Instant.now());
    }

    void close(ZLinkSpotCloseReason reason, Instant deadline) {
        try {
            notifyClosing(reason, deadline);
        } finally {
            closeResources();
        }
    }

    void notifyClosing(ZLinkSpotCloseReason reason, Instant deadline) {
        if (spot == null) {
            return;
        }
        host.awaitClosing(closingStage(reason, deadline));
    }

    CompletionStage<Void> closingStage(ZLinkSpotCloseReason reason, Instant deadline) {
        if (spot == null) {
            return CompletableFuture.completedFuture(null);
        }
        return closingCallback(
                () ->
                        context.enqueueLifecycle(
                                () ->
                                        host.runWithOutbound(
                                                context.dispatchOutbound(),
                                                () ->
                                                        ZLinkHandlerStages.fromStageSupplier(
                                                                () ->
                                                                        spot.onClosing(
                                                                                new ZLinkSpotClosingContext(
                                                                                        reason,
                                                                                        deadline))))));
    }

    private void closeResources() {
        closePendingActorMessage();
        closeActiveRouteReceives();
        context.closeTimers();
        context.closeHandlerInstances();
        backendSpot.close();
    }
}
