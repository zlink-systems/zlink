package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.*;
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
    private volatile String meshName;

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

    /** Records the MeshNode that admitted this User Spot activation (runtime monitoring §5). */
    void admittedBy(String meshName) {
        this.meshName = java.util.Objects.requireNonNull(meshName, "meshName");
    }

    String meshName() {
        return meshName;
    }

    @Override
    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail, Supplier<CompletionStage<Void>> operation) {
        return tail.thenCompose(ignored -> operation.get());
    }

    CompletionStage<Void> handleDispatchEvent(ZLinkBackendSpotDispatchInfo info) {
        if (info.event() == ZLinkBackendSpotDispatchEvent.ROUTED_READABLE) {
            return drainRoutesForDispatch();
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
        if (info.event() == ZLinkBackendSpotDispatchEvent.ACTOR_READABLE) {
            return dispatchActorMessages(info.actorMessages());
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

    CompletionStage<Void> admitRoute(ZLinkBackendReceived received) {
        return admitRoute(received, null);
    }

    CompletionStage<Void> admitRoute(
            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
        var dispatched =
                host.admitNewApplicationJob(
                        context,
                        () -> {
                            ZLinkFrameworkException hostRejection =
                                    host.spotHostAdmissionFailure(context.spotId());
                            if (hostRejection != null)
                                return CompletableFuture.failedFuture(hostRejection);
                            if (host.dispatchSpotRouteBridgePacket(received)) {
                                received.close();
                                if (admission != null) admission.complete(null);
                                return CompletableFuture.completedFuture(null);
                            }
                            var replyRoute =
                                    host.registerRelocationReplyLazy(
                                            () -> ZLinkSpotAcceptedJournal.encode(received),
                                            received,
                                            context.spotId(),
                                            backendSpot.lifecycleGeneration());
                            CompletionStage<Void> admitted;
                            try {
                                admitted =
                                        context.enqueueAcceptedDispatch(
                                                replyRoute::record,
                                                received.acceptedJournalRecordSize(),
                                                () ->
                                                        dispatchRouteAsync(received)
                                                                .whenComplete(
                                                                        (done, failure) ->
                                                                                replyRoute
                                                                                        .completeLocal()),
                                                replyRoute::releaseForRelocation,
                                                admission);
                            } catch (RuntimeException | Error failure) {
                                replyRoute.completeLocal();
                                throw failure;
                            }
                            return admitted.whenComplete(
                                    (done, failure) -> {
                                        if (failure != null) replyRoute.completeLocal();
                                    });
                        });
        dispatched.whenComplete(
                (done, failure) -> {
                    if (failure != null) {
                        if (admission != null) admission.completeExceptionally(failure);
                        received.close();
                    }
                });
        return dispatched;
    }

    Boolean admitTopic(ZLinkBackendTopicMessage message) {
        var permit = host.reserveApplicationJob();
        if (permit == null) {
            Thread.currentThread().interrupt();
            message.parts().forEach(Message::close);
            return false;
        }
        try (var ignored =
                systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext.enter(
                        permit)) {
            if (host.spotHostAdmissionFailure(context.spotId()) != null) {
                message.parts().forEach(Message::close);
                return false;
            }
            var ownership =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .transferToQueuedJob();
            boolean accepted;
            try {
                accepted =
                        context.tryEnqueueSpot(
                                () ->
                                        host.runQueuedApplicationJob(
                                                ownership,
                                                () -> dispatchSpotSubscription(message)));
            } catch (RuntimeException | Error failure) {
                ownership.close();
                message.parts().forEach(Message::close);
                throw failure;
            }
            if (!accepted) {
                ownership.close();
                message.parts().forEach(Message::close);
            }
            return accepted;
        } finally {
            permit.abandonReservation();
        }
    }

    CompletionStage<ZLinkSpotActorJoinResult> admitLocalActorJoin(
            java.util.function.Supplier<CompletionStage<ZLinkSpotActorJoinResult>> operation) {
        var permit = host.reserveApplicationJob();
        if (permit == null) {
            Thread.currentThread().interrupt();
            return CompletableFuture.failedFuture(
                    new IllegalStateException("application job reservation was interrupted"));
        }
        try (var ignored =
                systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext.enter(
                        permit)) {
            var rejection = host.spotHostAdmissionFailure(context.spotId());
            if (rejection != null) {
                return CompletableFuture.failedFuture(rejection);
            }
            var ownership =
                    systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                            .transferToQueuedJob();
            CompletableFuture<ZLinkSpotActorJoinResult> response = new CompletableFuture<>();
            CompletionStage<Void> admitted;
            try {
                admitted =
                        context.enqueueJoinLifecycle(
                                () ->
                                        host.runQueuedApplicationJob(ownership, operation)
                                                .thenAccept(response::complete));
            } catch (RuntimeException | Error failure) {
                ownership.close();
                throw failure;
            }
            return admitted.whenComplete(
                            (done, failure) -> {
                                if (failure != null) {
                                    ownership.close();
                                }
                            })
                    .thenCompose(done -> response);
        } finally {
            permit.abandonReservation();
        }
    }

    CompletionStage<Void> admitActor(List<ZLinkBackendActorReceived> messages) {
        var dispatched =
                dispatchActorMessages(
                        messages,
                        operation -> {
                            var hostRejection = host.spotHostAdmissionFailure(context.spotId());
                            return hostRejection == null
                                    ? context.admitIngress(operation)
                                    : CompletableFuture.failedFuture(hostRejection);
                        });
        dispatched.whenComplete(
                (done, failure) -> messages.forEach(ZLinkBackendActorReceived::close));
        return dispatched;
    }

    void drainPolledDispatchQueues() {
        drainRoutesForDispatch();
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
                        actor -> host.notifySpotActorLifecycle(spot, actor, true),
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

    CompletionStage<Void> closeAsync() {
        return closeAsync(ZLinkSpotCloseReason.EXPLICIT_CLOSE, Instant.now());
    }

    CompletionStage<Void> closeAsync(ZLinkSpotCloseReason reason, Instant deadline) {
        return closingStage(reason, deadline)
                .handle((ignored, failure) -> finishCleanup(failure, closeResourcesAsync()))
                .thenCompose(stage -> stage);
    }

    CompletionStage<Void> closingStage(ZLinkSpotCloseReason reason, Instant deadline) {
        if (spot == null) {
            return CompletableFuture.completedFuture(null);
        }
        Supplier<CompletionStage<Void>> callback =
                () ->
                        closingCallback(
                                () ->
                                        host.runWithOutbound(
                                                context.dispatchOutbound(),
                                                () ->
                                                        ZLinkHandlerStages.fromStageSupplier(
                                                                () ->
                                                                        spot.onClosing(
                                                                                new ZLinkSpotClosingContext(
                                                                                        reason,
                                                                                        deadline)))));
        return context.isCurrentSpotTurn()
                ? context.runLifecycleExecution(callback)
                : context.enqueueLifecycle(callback);
    }

    private CompletionStage<Void> closeResourcesAsync() {
        closePendingActorMessage();
        closeActiveRouteReceives();
        return context.closeResourcesAsync();
    }
}
