package systems.zlink.framework.runtime.spots;
import java.util.Collections;
import java.util.IdentityHashMap;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime.LocalActorReply;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

abstract class SpotActivationBase<C extends SpotDispatchLine> implements AutoCloseable {
    final ZLinkSpotRuntime host;
    final ZLinkSpotHandlerInvoker handlerInvoker;
    final Object spotSurface;
    final ZLinkBackendSpot backendSpot;
    final C context;
    private final Set<ZLinkBackendReceived> activeRouteReceives =
        Collections.synchronizedSet(
            Collections.newSetFromMap(new IdentityHashMap<>()));
    private ZLinkBackendActorReceived pendingActorHeader;

    SpotActivationBase(
        ZLinkSpotRuntime host,
        ZLinkSpotHandlerInvoker handlerInvoker,
        Object spotSurface,
        ZLinkBackendSpot backendSpot,
        C context) {
        this.host = host;
        this.handlerInvoker = handlerInvoker;
        this.spotSurface = spotSurface;
        this.backendSpot = backendSpot;
        this.context = context;
    }

    abstract CompletionStage<Void> appendSpotHandler(
        CompletionStage<Void> tail,
        Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> appendSpotHandler(
        CompletionStage<Void> tail,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return appendSpotHandler(tail, operation);
    }

    abstract CompletionStage<Void> appendActorLifecycle(
        CompletionStage<Void> tail,
        ZLinkBackendActorLifecycleEvent event,
        ZLinkBackendActorRef actorRef,
        ZLinkActor actor);

    final void trackRouteReceived(ZLinkBackendReceived received) {
        activeRouteReceives.add(received);
    }

    final boolean hasActiveRouteReceives() {
        return !activeRouteReceives.isEmpty();
    }

    CompletionStage<Void> dispatchResolvedActorPacket(
        ZLinkActor actor,
        ActorPacketFrames.Header packetHeader,
        ActorMessageRead read) {
        return host.dispatchLocalActorPacket(
            context,
            spotSurface,
            actor,
            packetHeader,
            read.headerPart(),
            read.bodyPart(),
            read.fromPendingHeader());
    }

    final CompletionStage<Void> dispatchActorMessages(
        List<ZLinkBackendActorReceived> actorMessages) {
        List<CompletionStage<Void>> dispatches = new ArrayList<>();
        int index = 0;
        while (index < actorMessages.size() || pendingActorHeader != null) {
            ActorMessageRead read = host.readActorMessage(
                actorMessages,
                index,
                pendingActorHeader);
            index = read.nextIndex();
            pendingActorHeader = read.nextPendingHeader();
            if (!read.complete()) {
                return CompletableFuture.completedFuture(null);
            }
            boolean pendingHeader = read.fromPendingHeader();
            ZLinkBackendActorReceived headerPart = read.headerPart();
            ZLinkBackendActorReceived bodyPart = read.bodyPart();
            ActorPacketFrames.Header packetHeader = ActorPacketFrames.decode(headerPart);
            if (!host.actorSessions().available()) {
                host.closePendingActorHeader(headerPart, pendingHeader);
                continue;
            }
            Optional<ZLinkActor> localActor =
                host.actorSessions().localActor(headerPart.actor().actorId());
            if (localActor.isEmpty()) {
                host.reportSpotActorHandlerMissing(
                    packetHeader,
                    context.spotId(),
                    headerPart.actor().actorId(),
                    headerPart.sourceNodeRid());
                if (packetHeader.requestSeq().isPresent()) {
                    ZLinkBackendActorReceived headerCopy = pendingHeader
                        ? headerPart
                        : host.copyActorReceived(headerPart);
                    host.replyActorDispatchError(
                        context,
                        packetHeader,
                        headerCopy,
                        headerPart.actor().actorId(),
                        new ZLinkConfigurationException(
                            "SPOT actor is not registered locally: "
                                + headerPart.actor().actorId()),
                        "actor missing error reply failed");
                } else {
                    host.closePendingActorHeader(headerPart, pendingHeader);
                }
                continue;
            }
            ZLinkActor actor = localActor.get();
            if (host.dispatchActorControlPacket(packetHeader, headerPart, actor, pendingHeader)) {
                continue;
            }
            // Admit every Actor packet to its own Actor queue before waiting
            // for any handler stage. The Spot-wide execution gate is acquired
            // by the queued turn, so a yielded Actor does not block admission
            // of another Actor's turn.
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                host.closePendingActorHeader(headerPart, pendingHeader);
                continue;
            }
            try (var ignored = systems.zlink.framework.runtime.internal.dispatch
                     .ZLinkApplicationJobContext.enter(permit)) {
                dispatches.add(dispatchResolvedActorPacket(
                    actor, packetHeader, read));
            } finally {
                permit.abandonReservation();
            }
        }
        return CompletableFuture.allOf(
            dispatches.stream()
                .map(CompletionStage::toCompletableFuture)
                .toArray(CompletableFuture[]::new));
    }

    final CompletionStage<Void> drainActorLifecycleEvents() {
        CompletionStage<Void> tail =
            CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            if (host.isClosing()) {
                return tail;
            }
            var permit = host.reserveApplicationJob();
            if (permit == null) {
                return tail;
            }
            systems.zlink.framework.runtime.internal.dispatch
                .ZLinkApplicationJobContext.QueuedOwnership ownership = null;
            ZLinkBackendActorLifecycleEvent event;
            ZLinkBackendActorRef actorRef = null;
            Optional<ZLinkActor> actor = Optional.empty();
            try (var ignored = systems.zlink.framework.runtime.internal.dispatch
                     .ZLinkApplicationJobContext.enter(permit)) {
                event = backendSpot.recvActorLifecycle(
                    ZLinkBackendRecvMode.DONT_WAIT);
                if (event == null) {
                    return tail;
                }
                batch.record(0);
                if (host.actorSessions().available()) {
                    actorRef = host.actorLifecycleRef(event);
                    actor = host.actorSessions().localActor(actorRef.actorId());
                    if (actor.isPresent()) {
                        ownership = systems.zlink.framework.runtime.internal.dispatch
                            .ZLinkApplicationJobContext.transferToQueuedJob();
                    }
                }
            } finally {
                permit.abandonReservation();
            }
            if (ownership != null) {
                CompletionStage<Void> prior = tail;
                var queuedOwnership = ownership;
                var capturedActorRef = actorRef;
                ZLinkActor capturedActor = actor.orElseThrow();
                tail = prior.thenCompose(ignored -> host.runQueuedApplicationJob(
                    queuedOwnership,
                    () -> appendActorLifecycle(
                        CompletableFuture.completedFuture(null),
                        event,
                        capturedActorRef,
                        capturedActor)));
            }
        }
        return tail;
    }

    final CompletionStage<Void> dispatchSpotRouteHandler(
        ZLinkBackendReceived received,
        ParsedPacket packet) {
        Map<String, String> metadata;
        try {
            metadata = ZLinkApplicationMetadata.decode(
                received.applicationMetadata());
        } catch (IllegalArgumentException error) {
            if (received.requestSeq().isPresent()) {
                host.replySpotRouteDispatchError(
                    received,
                    packet.packetName(),
                    context.spotId(),
                    ZLinkDispatchErrorReason.INVALID_FRAME,
                    error);
            } else {
                host.reportSpotRouteSendDropped(
                    received, packet.packetName(), context.spotId());
            }
            closeRouteReceived(received);
            return CompletableFuture.completedFuture(null);
        }
        SpotPacketHandlerRegistration handler =
            context.handlerCatalog().packetHandler(packet.packetName());
        if (handler == null) {
            if (received.requestSeq().isPresent()) {
                host.replySpotRouteDispatchError(
                    received,
                    packet.packetName(),
                    context.spotId(),
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    null);
            } else {
                host.reportSpotRouteSendDropped(received, packet.packetName(), context.spotId());
            }
            closeRouteReceived(received);
            return CompletableFuture.completedFuture(null);
        }
        if (received.requestSeq().isPresent()) {
            if (!handler.request()) {
                host.replySpotRouteDispatchError(
                    received,
                    packet.packetName(),
                    context.spotId(),
                    ZLinkDispatchErrorReason.HANDLER_MISSING,
                    null);
                closeRouteReceived(received);
                return CompletableFuture.completedFuture(null);
            }
            Message payloadCopy = Message.from(packet.payload());
            return host.admitApplicationJob(() -> {
                host.traceSpotRouteFlow(
                    ZLinkMessageFlowOutcome.ADMITTED,
                    ZLinkDispatchMessageKind.REQUEST,
                    packet.packetName(),
                    received.requestSeq(),
                    context.spotId());
                return appendSpotHandler(
                    CompletableFuture.completedFuture(null),
                    payloadCopy.size(),
                    () -> {
                        try {
                        host.traceSpotRouteFlow(
                            ZLinkMessageFlowOutcome.DISPATCHED,
                            ZLinkDispatchMessageKind.REQUEST,
                            packet.packetName(),
                            received.requestSeq(),
                            context.spotId());
                        systems.zlink.framework.runtime.internal.dispatch
                            .ZLinkApplicationJobContext
                            .beforeFirstApplicationInstruction();
                        return ZLinkFlowContext.propagate(
                            host.runWithOutbound(context.dispatchOutbound(), () ->
                                handlerInvoker.invokeRequest(
                                    handler,
                                    spotSurface,
                                    payloadCopy,
                                    received.contentType(),
                                    metadata,
                                    context.handlerInstances()::instance))
                                .thenAccept(reply -> received.reply(List.of(reply))));
                        } catch (RuntimeException failure) {
                            return CompletableFuture.failedFuture(failure);
                        }
                    });
                })
                      .whenComplete((ignored, error) -> {
                        if (error != null && !host.isClosing()) {
                            host.replySpotRouteDispatchError(
                                received,
                                packet.packetName(),
                                context.spotId(),
                                ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                                error);
                        }
                        payloadCopy.close();
                        closeRouteReceived(received);
                        if (error == null) {
                            host.traceSpotRouteFlow(
                                ZLinkMessageFlowOutcome.REPLIED,
                                ZLinkDispatchMessageKind.REQUEST,
                                packet.packetName(),
                                received.requestSeq(),
                                context.spotId());
                        }
                      });
        }
        if (handler.request()) {
            host.reportSpotRouteSendDropped(received, packet.packetName(), context.spotId());
            closeRouteReceived(received);
            return CompletableFuture.completedFuture(null);
        }
        Message payloadCopy = Message.from(packet.payload());
        String packetName = packet.packetName();
        releaseRouteParts(received);
        return host.admitApplicationJob(() -> {
            host.traceSpotRouteFlow(
                ZLinkMessageFlowOutcome.ADMITTED,
                ZLinkDispatchMessageKind.SEND,
                packetName,
                Optional.empty(),
                context.spotId());
            return appendSpotHandler(
                CompletableFuture.completedFuture(null),
                payloadCopy.size(),
                () -> {
                    host.traceSpotRouteFlow(
                        ZLinkMessageFlowOutcome.DISPATCHED,
                        ZLinkDispatchMessageKind.SEND,
                        packetName,
                        Optional.empty(),
                        context.spotId());
                    return startSpotHandler(() ->
                        host.runWithOutbound(context.dispatchOutbound(), () ->
                            handlerInvoker.invokePacket(
                                handler,
                                spotSurface,
                                payloadCopy,
                                received.contentType(),
                                metadata,
                                context.handlerInstances()::instance)));
                });
            })
                .whenComplete((ignored, error) -> {
                    payloadCopy.close();
                    if (error == null) {
                        host.traceSpotRouteFlow(
                            ZLinkMessageFlowOutcome.COMPLETED,
                            ZLinkDispatchMessageKind.SEND,
                            packetName,
                            Optional.empty(),
                            context.spotId());
                    }
                });
    }

    final CompletionStage<Void> handleRoutedBoundSessionSendParts(
        List<Message> parts) {
        List<Message> packetParts = parts.size() == 2
            ? systems.zlink.framework.runtime.actors
                .ZLinkActorEntryTransferEnvelope.decode(parts.get(1))
            : parts;
        boolean ownsPacketParts = packetParts != parts;
        ZLinkActorSpotRoutePackets.BoundSessionSend send;
        try {
            send = ZLinkActorSpotRoutePackets.decodeBoundSessionSend(
                packetParts);
        } finally {
            if (ownsPacketParts) {
                packetParts.forEach(Message::close);
            }
        }
        byte[] frameBytes = send.frame().toByteArray();
        CompletionStage<Void> sendStage = host.actorSessions().sendBoundSession(
            send.actorRef(),
            frameBytes,
            () -> host.sendActorBoundSessionWithRetry(
                host.primaryNode(),
                send.actorRef(),
                send.actorRef().actorId(),
                frameBytes,
                "routed actor bound session send failed"));
        return sendStage.whenComplete((ignored, error) -> send.close());
    }

    final CompletionStage<List<Message>> handleRoutedBoundSessionSendRequestParts(
        List<Message> parts) {
        return handleRoutedBoundSessionSendParts(parts)
            .thenApply(ignored -> List.of(Message.from(new byte[0])));
    }

    final CompletionStage<Optional<Message>> handleRoutedActorPacketParts(
        List<Message> parts) {
        List<Message> packetParts = parts.size() == 2
            ? systems.zlink.framework.runtime.actors
                .ZLinkActorEntryTransferEnvelope.decode(parts.get(1))
            : parts;
        ZLinkActorSpotRoutePackets.ActorPacket packet;
        try {
            packet = ZLinkActorSpotRoutePackets.decodeActorPacket(packetParts);
        } finally {
            if (packetParts != parts) {
                packetParts.forEach(Message::close);
            }
        }
        if (packet.handoffArrivalIndex() != null) {
            host.actorAdmissions().traceTransferMarker(
                "backlog_enqueued",
                packet.actorRef().actorId(),
                packet.handoffArrivalIndex());
        }
        CompletionStage<Optional<LocalActorReply>> dispatched =
            packet.handoffArrivalIndex() == null
                ? packet.acceptedJournalRecord().length == 0
                    ? host.dispatchLocalSessionActor(
                        packet.actorRef(),
                        packet.header(),
                        packet.payload())
                    : host.dispatchMessageFollow(
                        packet.actorRef(),
                        packet.header(),
                        packet.payload(),
                        packet.acceptedJournalRecord())
                : host.dispatchTransferBacklog(
                    packet.actorRef(),
                    packet.header(),
                    packet.payload(),
                    packet.acceptedJournalRecord())
                    .thenApply(reply -> reply.map(message -> new LocalActorReply(
                        message,
                        packet.header().codec())));
        return dispatched.thenCompose(reply ->
            host.replyTransferredRequestDirect(
                    packet.actorRef(), packet.header(), packet.replyRoute(), reply)
            .thenApply(completed -> {
                if (packet.replyRoute() != null) {
                    return Optional.of(
                        ZLinkActorSpotRoutePackets.createHandoffDirectReplyAck());
                }
                if (completed.isEmpty()) {
                    return Optional.<Message>empty();
                }
                LocalActorReply actorReply = completed.orElseThrow();
                try (Message ignored = actorReply.payload()) {
                    return Optional.of(ActorPacketFrames.encodeRoutedReply(
                        packet.header(), actorReply));
                }
            }))
            .whenComplete((ignored, error) -> packet.close());
    }

    final CompletionStage<Void> dispatchSpotSubscription(
        ZLinkBackendTopicMessage received) {
        boolean leaseReleaseScheduled = false;
        try {
            if (host.isDraining()) {
                return CompletableFuture.completedFuture(null);
            }
            if (received.parts().isEmpty()) {
                host.reportSpotSubscriptionDropped(
                    received.topic(),
                    null,
                    context.spotId(),
                    ZLinkDispatchErrorReason.INVALID_FRAME);
                return CompletableFuture.completedFuture(null);
            }
            ParsedPacket packet = host.parsePacket(received.parts());
            Map<String, String> metadata;
            try {
                metadata = ZLinkApplicationMetadata.decode(
                    received.applicationMetadata());
            } catch (IllegalArgumentException error) {
                host.reportSpotSubscriptionDropped(
                    received.topic(),
                    null,
                    context.spotId(),
                    ZLinkDispatchErrorReason.INVALID_FRAME);
                return CompletableFuture.completedFuture(null);
            }
            CompletionStage<Void> tail =
                CompletableFuture.completedFuture(null);
            List<SpotSubscriptionHandlerRegistration> matchingHandlers =
                context.handlerCatalog().subscriptionHandlers(received.topic()).stream()
                    .filter(handler -> handler.packetName().equals(packet.packetName()))
                    .toList();
            if (!matchingHandlers.isEmpty()) {
                ZLinkInboundPayloadOwner payloadOwner = handlerInvoker.payloadOwner(
                    packet.payload(), received.contentType());
                Object decoded = handlerInvoker.deserializeSubscription(
                    matchingHandlers.get(0), payloadOwner);
                boolean reuseIngressPermit = systems.zlink.framework.runtime
                    .internal.dispatch.ZLinkApplicationJobContext
                    .current().isPresent();
                for (int index = 0; index < matchingHandlers.size(); index++) {
                    SpotSubscriptionHandlerRegistration handler =
                        matchingHandlers.get(index);
                    CompletionStage<Void> prior = tail;
                    Supplier<CompletionStage<Void>> job = () ->
                        appendSpotHandler(
                            prior,
                            packet.payload().size(),
                            () -> startSpotHandler(() ->
                                host.runWithOutbound(
                                    context.dispatchOutbound(),
                                    () -> handlerInvoker.invokeSubscriptionDecoded(
                                        handler,
                                        spotSurface,
                                        received.channelName(),
                                        received.topic(),
                                        received.routingId().map(Object::toString),
                                        decoded,
                                        received.contentType(),
                                        metadata,
                                        context.handlerInstances()::instance))));
                    tail = reuseIngressPermit && index == 0
                        ? job.get()
                        : host.admitNewApplicationJob(job);
                }
            } else {
                host.reportSpotSubscriptionDropped(
                    received.topic(),
                    packet.packetName(),
                    context.spotId(),
                    ZLinkDispatchErrorReason.HANDLER_MISSING);
            }
            return tail;
        } finally {
            received.parts().forEach(Message::close);
            }
    }

    final void closeActiveRouteReceives() {
        for (ZLinkBackendReceived received : List.copyOf(activeRouteReceives)) {
            closeRouteReceived(received);
        }
    }

    final void closeRouteReceived(ZLinkBackendReceived received) {
        if (activeRouteReceives.remove(received)) {
            received.close();
        }
    }

    final void releaseRouteParts(ZLinkBackendReceived received) {
        if (activeRouteReceives.remove(received)) {
            received.closeParts();
        }
    }

    final void closePendingActorMessage() {
        if (pendingActorHeader != null) {
            pendingActorHeader.close();
            pendingActorHeader = null;
        }
    }

    private CompletionStage<Void> startSpotHandler(
        Supplier<CompletionStage<Void>> operation) {
        systems.zlink.framework.runtime.internal.dispatch
            .ZLinkApplicationJobContext.beforeFirstApplicationInstruction();
        return operation.get();
    }
}
