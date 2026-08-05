package systems.zlink.framework.runtime.spots;

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
import systems.zlink.framework.configuration.ZLinkDispatchErrorReason;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkInboundDispatchBudget;
import systems.zlink.framework.runtime.internal.dispatch.ZLinkReceiveBatchBudget;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

abstract class SpotActivationBase<C extends SpotDispatchLine> implements AutoCloseable {
    final ZLinkSpotRuntime host;
    final ZLinkSpotHandlerInvoker handlerInvoker;
    final Object spotSurface;
    final ZLinkBackendSpot backendSpot;
    final C context;
    private final Set<ZLinkBackendReceived> activeRouteReceives =
        java.util.Collections.synchronizedSet(
            java.util.Collections.newSetFromMap(new java.util.IdentityHashMap<>()));
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
                return java.util.concurrent.CompletableFuture.completedFuture(null);
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
            dispatches.add(dispatchResolvedActorPacket(
                actor, packetHeader, read));
        }
        return CompletableFuture.allOf(
            dispatches.stream()
                .map(CompletionStage::toCompletableFuture)
                .toArray(CompletableFuture[]::new));
    }

    final CompletionStage<Void> drainActorLifecycleEvents() {
        CompletionStage<Void> tail =
            java.util.concurrent.CompletableFuture.completedFuture(null);
        ZLinkReceiveBatchBudget batch = new ZLinkReceiveBatchBudget();
        while (batch.canReceiveNext()) {
            if (host.isClosing()) {
                return tail;
            }
            ZLinkBackendActorLifecycleEvent event =
                backendSpot.recvActorLifecycle(ZLinkBackendRecvMode.DONT_WAIT);
            if (event == null) {
                return tail;
            }
            batch.record(0);
            if (!host.actorSessions().available()) {
                continue;
            }
            ZLinkBackendActorRef actorRef = host.actorLifecycleRef(event);
            Optional<ZLinkActor> actor = host.actorSessions().localActor(actorRef.actorId());
            if (actor.isPresent()) {
                tail = appendActorLifecycle(tail, event, actorRef, actor.get());
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
            return java.util.concurrent.CompletableFuture.completedFuture(null);
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
            return java.util.concurrent.CompletableFuture.completedFuture(null);
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
                return java.util.concurrent.CompletableFuture.completedFuture(null);
            }
            Message payloadCopy = Message.from(packet.payload());
            ZLinkInboundDispatchBudget.Lease lease =
                received.inboundDispatchLease() != null
                    ? received.inboundDispatchLease()
                    : host.inboundDispatchBudget().track(payloadCopy.size());
            return appendSpotHandler(
                java.util.concurrent.CompletableFuture.completedFuture(null),
                payloadCopy.size(),
                () -> host.inboundDispatchBudget().acquireCompletionPermit()
                    .thenCompose(permit -> {
                        try {
                            lease.handlerStarted();
                            return systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext.propagate(
                                host.runWithOutbound(context.dispatchOutbound(), () ->
                                    handlerInvoker.invokeRequest(
                                        handler,
                                        spotSurface,
                                        payloadCopy,
                                        received.contentType(),
                                        metadata,
                                        context.handlerInstances()::instance))
                                    .thenAccept(reply -> received.reply(List.of(reply))))
                                .whenComplete((ignored, error) -> permit.close());
                        } catch (RuntimeException failure) {
                            permit.close();
                            return CompletableFuture.failedFuture(failure);
                        }
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
                        lease.close();
                        closeRouteReceived(received);
                        if (error == null) {
                            host.traceMessageFlow(
                                ZLinkMessageFlowOutcome.REPLIED,
                                ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                ZLinkDispatchMessageKind.REQUEST,
                                packet.packetName(),
                                null,
                                null,
                                received.requestSeq().map(String::valueOf).orElse(null),
                                null,
                                context.spotId().toString(),
                                null);
                        }
                    }));
        }
        if (handler.request()) {
            host.reportSpotRouteSendDropped(received, packet.packetName(), context.spotId());
            closeRouteReceived(received);
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        Message payloadCopy = Message.from(packet.payload());
        ZLinkInboundDispatchBudget.Lease lease =
            received.inboundDispatchLease() != null
                ? received.inboundDispatchLease()
                : host.inboundDispatchBudget().track(payloadCopy.size());
        String packetName = packet.packetName();
        releaseRouteParts(received);
        return appendSpotHandler(
            java.util.concurrent.CompletableFuture.completedFuture(null),
            payloadCopy.size(),
            () ->
            startSpotHandler(lease, () ->
                host.runWithOutbound(context.dispatchOutbound(), () ->
                    handlerInvoker.invokePacket(
                        handler,
                        spotSurface,
                        payloadCopy,
                        received.contentType(),
                        metadata,
                        context.handlerInstances()::instance)))
                .whenComplete((ignored, error) -> {
                    payloadCopy.close();
                    lease.close();
                    received.closeAdmission();
                    if (error == null) {
                        host.traceMessageFlow(
                            ZLinkMessageFlowOutcome.DISPATCHED,
                            ZLinkDispatchErrorSurface.SPOT_ROUTE,
                            ZLinkDispatchMessageKind.SEND,
                            packetName,
                            null,
                            null,
                            null,
                            null,
                            context.spotId().toString(),
                            null);
                    }
                }));
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
        CompletionStage<Optional<Message>> dispatched =
            packet.handoffArrivalIndex() == null
                ? host.dispatchLocalSessionActor(
                    packet.actorRef(),
                    packet.header(),
                    packet.payload())
                : host.dispatchTransferBacklog(
                    packet.actorRef(),
                    packet.header(),
                    packet.payload(),
                    packet.acceptedJournalRecord());
        return dispatched
            .thenApply(reply -> {
                Optional<Message> completed = host.replyTransferredRequestDirect(
                    packet.actorRef(), packet.header(), packet.replyRoute(), reply);
                return packet.replyRoute() == null
                    ? completed
                    : Optional.of(ZLinkActorSpotRoutePackets.createHandoffDirectReplyAck());
            })
            .whenComplete((ignored, error) -> packet.close());
    }

    final CompletionStage<Void> dispatchSpotSubscription(
        ZLinkBackendTopicMessage received) {
        boolean dispatched = false;
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
                return java.util.concurrent.CompletableFuture.completedFuture(null);
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
                return java.util.concurrent.CompletableFuture.completedFuture(null);
            }
            host.traceMessageFlow(
                ZLinkMessageFlowOutcome.RECEIVED,
                ZLinkDispatchErrorSurface.SPOT_SUBSCRIPTION,
                ZLinkDispatchMessageKind.PUBLISH,
                packet.packetName(),
                null,
                received.topic(),
                null,
                null,
                context.spotId().toString(),
                null);
            CompletionStage<Void> tail =
                java.util.concurrent.CompletableFuture.completedFuture(null);
            ZLinkInboundDispatchBudget.Lease sharedLease =
                received.inboundDispatchLease();
            for (SpotSubscriptionHandlerRegistration handler :
                context.handlerCatalog().subscriptionHandlers(received.topic())) {
                if (!handler.packetName().equals(packet.packetName())) {
                    continue;
                }
                dispatched = true;
                Message payloadCopy = Message.from(packet.payload());
                ZLinkInboundDispatchBudget.Lease lease =
                    received.inboundDispatchLease() != null
                        ? received.inboundDispatchLease()
                        : host.inboundDispatchBudget().track(payloadCopy.size());
                tail = appendSpotHandler(
                    tail,
                    payloadCopy.size(),
                    () -> startSpotHandler(lease, () ->
                        host.runWithOutbound(context.dispatchOutbound(), () ->
                            handlerInvoker.invokeSubscription(
                                handler,
                                spotSurface,
                                received.channelName(),
                                received.topic(),
                                received.routingId().map(Object::toString),
                                payloadCopy,
                                received.contentType(),
                                metadata,
                                context.handlerInstances()::instance)))
                        .whenComplete((ignored, error) -> payloadCopy.close()));
                if (sharedLease == null) {
                    tail = tail.whenComplete((ignored, error) -> lease.close());
                }
            }
            if (dispatched) {
                if (sharedLease != null) {
                    tail = tail.whenComplete(
                        (ignored, error) -> sharedLease.close());
                }
                host.traceMessageFlow(
                    ZLinkMessageFlowOutcome.DISPATCHED,
                    ZLinkDispatchErrorSurface.SPOT_SUBSCRIPTION,
                    ZLinkDispatchMessageKind.PUBLISH,
                    packet.packetName(),
                    null,
                    received.topic(),
                    null,
                    null,
                    context.spotId().toString(),
                    null);
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
            if (!dispatched || received.inboundDispatchLease() == null) {
                received.closeAdmission();
            }
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
        ZLinkInboundDispatchBudget.Lease lease,
        Supplier<CompletionStage<Void>> operation) {
        lease.handlerStarted();
        return operation.get();
    }
}
