package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.actors.ZLinkActorSpotRoutePackets;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorsRuntime.LocalActorReply;
import systems.zlink.framework.runtime.internal.ZLinkCompletionBridge;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.runtime.messaging.ZLinkApplicationMetadata;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Collections;
import java.util.IdentityHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

abstract class SpotActivationBase<C extends SpotDispatchLine> {
    static <T> CompletionStage<T> finishCleanup(Throwable failure, CompletionStage<T> cleanup) {
        Throwable primary = unwrapCompletion(failure);
        return cleanup.handle(
                (value, cleanupFailure) -> {
                    Throwable secondary = unwrapCompletion(cleanupFailure);
                    if (primary != null) {
                        if (secondary != null && secondary != primary)
                            primary.addSuppressed(secondary);
                        throw new CompletionException(primary);
                    }
                    if (secondary != null) throw new CompletionException(secondary);
                    return value;
                });
    }

    private static Throwable unwrapCompletion(Throwable failure) {
        while (failure instanceof CompletionException && failure.getCause() != null) {
            failure = failure.getCause();
        }
        return failure;
    }

    final ZLinkSpotRuntime host;
    final ZLinkSpotHandlerInvoker handlerInvoker;
    final Object spotSurface;
    final ZLinkBackendSpot backendSpot;
    final C context;
    private final Set<ZLinkBackendReceived> activeRouteReceives =
            Collections.synchronizedSet(Collections.newSetFromMap(new IdentityHashMap<>()));
    private ZLinkBackendActorReceived pendingActorHeader;
    private CompletableFuture<Void> closingCallback;
    private volatile ZLinkSpotCloseCoordinator closeCoordinator;

    final synchronized ZLinkSpotCloseCoordinator closeCoordinator(
            Supplier<ZLinkSpotCloseCoordinator> create) {
        if (closeCoordinator == null) {
            closeCoordinator = Objects.requireNonNull(create.get(), "close coordinator");
        }
        return closeCoordinator;
    }

    final boolean hasCommittedClose() {
        ZLinkSpotCloseCoordinator coordinator = closeCoordinator;
        return coordinator != null && coordinator.committed();
    }

    final synchronized boolean closeStarted() {
        return closeCoordinator != null;
    }

    final synchronized ZLinkSpotCloseCoordinator existingCloseCoordinator() {
        return closeCoordinator;
    }

    final synchronized void clearUncommittedClose(ZLinkSpotCloseCoordinator candidate) {
        if (closeCoordinator == candidate && !candidate.committed()) {
            closeCoordinator = null;
        }
    }

    final CompletionStage<Void> closingCallback(Supplier<CompletionStage<Void>> callback) {
        CompletableFuture<Void> completion;
        synchronized (this) {
            if (closingCallback != null) {
                return closingCallback;
            }
            closingCallback = completion = new CompletableFuture<>();
        }
        try {
            CompletionStage<Void> stage = Objects.requireNonNull(callback.get(), "OnClosing stage");
            ZLinkCompletionBridge.forwardCancellation(completion, stage);
            stage.whenComplete(
                    (ignored, failure) -> {
                        if (failure == null) completion.complete(null);
                        else completion.completeExceptionally(failure);
                    });
        } catch (Throwable failure) {
            completion.completeExceptionally(failure);
        }
        return completion;
    }

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
            CompletionStage<Void> tail, Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation) {
        return appendSpotHandler(tail, operation);
    }

    CompletionStage<Void> appendSpotHandler(
            CompletionStage<Void> tail,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission) {
        CompletionStage<Void> submitted = appendSpotHandler(tail, payloadBytes, operation);
        if (admission != null) admission.complete(null);
        return submitted;
    }

    final void trackRouteReceived(ZLinkBackendReceived received) {
        activeRouteReceives.add(received);
    }

    /**
     * Spec 27 §3: malformed flow information on a SPOT route envelope completes the request as
     * ProtocolError and drops a one-way message.
     */
    final void failRouteInvalidFlow(
            ZLinkBackendReceived received, ZLinkFrameworkException invalidFlow) {
        String packetName =
                received.parts().isEmpty() ? null : received.parts().getFirst().toUtf8String();
        if (received.requestSeq().isPresent()) {
            host.replySpotRouteDispatchError(
                    received,
                    packetName,
                    backendSpot.spotId(),
                    ZLinkDispatchErrorReason.INVALID_FRAME,
                    invalidFlow);
        } else {
            host.reportSpotRouteInvalidFrameDropped(
                    received, packetName, backendSpot.spotId(), invalidFlow);
        }
        closeRouteReceived(received);
    }

    CompletionStage<Void> dispatchResolvedActorPacket(
            ZLinkActor actor,
            ActorPacketFrames.Header packetHeader,
            ActorMessageRead read,
            CompletableFuture<Void> requestAdmission) {
        return host.dispatchLocalActorPacket(
                context,
                actor,
                packetHeader,
                read.headerPart(),
                read.bodyPart(),
                read.fromPendingHeader(),
                requestAdmission);
    }

    final CompletionStage<Void> admitActor(List<ZLinkBackendActorReceived> messages) {
        return admitActor(messages, null);
    }

    final CompletionStage<Void> admitActor(
            List<ZLinkBackendActorReceived> messages, CompletableFuture<Void> requestAdmission) {
        var dispatched = dispatchActorMessages(messages, this::admitActorIngress, requestAdmission);
        dispatched.whenComplete(
                (ignored, failure) -> messages.forEach(ZLinkBackendActorReceived::close));
        return dispatched;
    }

    CompletionStage<Void> admitActorIngress(Supplier<CompletionStage<Void>> operation) {
        return operation.get();
    }

    final CompletionStage<Void> dispatchActorMessages(
            List<ZLinkBackendActorReceived> actorMessages) {
        return dispatchActorMessages(actorMessages, Supplier::get);
    }

    final CompletionStage<Void> dispatchActorMessages(
            List<ZLinkBackendActorReceived> actorMessages,
            java.util.function.Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>>
                    admission) {
        return dispatchActorMessages(actorMessages, admission, null);
    }

    final CompletionStage<Void> dispatchActorMessages(
            List<ZLinkBackendActorReceived> actorMessages,
            java.util.function.Function<Supplier<CompletionStage<Void>>, CompletionStage<Void>>
                    admission,
            CompletableFuture<Void> requestAdmission) {
        List<CompletionStage<Void>> dispatches = new ArrayList<>();
        int index = 0;
        try {
            while (index < actorMessages.size() || pendingActorHeader != null) {
                ActorMessageRead read =
                        host.readActorMessage(actorMessages, index, pendingActorHeader);
                index = read.nextIndex();
                pendingActorHeader = read.nextPendingHeader();
                if (!read.complete()) {
                    break;
                }
                boolean pendingHeader = read.fromPendingHeader();
                ZLinkBackendActorReceived headerPart = read.headerPart();
                ZLinkBackendActorReceived bodyPart = read.bodyPart();
                try {
                    ActorPacketFrames.Header packetHeader;
                    try {
                        packetHeader = ActorPacketFrames.decode(headerPart);
                    } catch (RuntimeException invalidFrame) {
                        try (headerPart;
                                bodyPart) {
                            if (host.dispatchErrorsEnabled()) {
                                host.reportDispatchError(
                                        DispatchFailureReport.of(
                                                        ZLinkDispatchErrorSurface.SPOT_ACTOR,
                                                        ZLinkDispatchMessageKind.ACTOR_SEND,
                                                        ZLinkDispatchErrorReason.INVALID_FRAME,
                                                        ZLinkDispatchErrorAction.DROP)
                                                .spotId(context.spotId())
                                                .actorId(headerPart.actor().actorId())
                                                .sourceRid(headerPart.sourceNodeRid())
                                                .error(invalidFrame));
                            }
                        }
                        continue;
                    }
                    if (!host.actorSessions().available()) {
                        host.closePendingActorHeader(headerPart, pendingHeader);
                        continue;
                    }
                    Optional<ZLinkActor> localActor =
                            host.actorSessions().localActor(headerPart.actor().actorId());
                    if (localActor.isEmpty()) {
                        host.reportSpotActorDispatchFailure(
                                packetHeader,
                                ZLinkDispatchErrorReason.STALE_TARGET,
                                context.spotId(),
                                headerPart.actor().actorId(),
                                headerPart.sourceNodeRid());
                        if (packetHeader.requestSeq().isPresent()) {
                            ZLinkBackendActorReceived headerCopy =
                                    pendingHeader ? headerPart : host.copyActorReceived(headerPart);
                            host.replyActorDispatchError(
                                    context,
                                    packetHeader,
                                    headerCopy,
                                    headerPart.actor().actorId(),
                                    new ZLinkConfigurationException(
                                            "SPOT actor is not registered locally: "
                                                    + headerPart.actor().actorId()));
                        } else {
                            host.closePendingActorHeader(headerPart, pendingHeader);
                        }
                        continue;
                    }
                    ZLinkActor actor = localActor.get();
                    var control =
                            host.dispatchActorControlPacket(
                                    packetHeader, headerPart, bodyPart, actor, pendingHeader);
                    if (control != null) {
                        dispatches.add(control);
                        continue;
                    }
                    // Admit every Actor packet to its own Actor queue before waiting
                    // for any handler stage. The Spot-wide execution gate is acquired
                    // by the queued turn, so a yielded Actor does not block admission
                    // of another Actor's turn.
                    var dispatched =
                            host.admitNewApplicationJob(
                                    context,
                                    () ->
                                            admission.apply(
                                                    () ->
                                                            dispatchResolvedActorPacket(
                                                                    actor,
                                                                    packetHeader,
                                                                    read,
                                                                    requestAdmission)));
                    dispatched.whenComplete(
                            (done, failure) ->
                                    host.closePendingActorHeader(headerPart, pendingHeader));
                    dispatches.add(dispatched);
                } catch (RuntimeException | Error failure) {
                    host.closePendingActorHeader(headerPart, pendingHeader);
                    throw failure;
                }
            }
        } catch (RuntimeException | Error failure) {
            if (pendingActorHeader != null) {
                pendingActorHeader.close();
                pendingActorHeader = null;
            }
            dispatches.add(CompletableFuture.failedFuture(failure));
        }
        var futures =
                dispatches.stream()
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new);
        CompletableFuture<Void> completion =
                systems.zlink.framework.runtime.internal.ZLinkCompletionBridge
                        .newCancellationFuture(
                                failure -> {
                                    if (failure != null)
                                        for (var future : futures) future.cancel(false);
                                });
        CompletableFuture.allOf(futures)
                .whenComplete(
                        (done, failure) -> {
                            if (failure == null) completion.complete(null);
                            else completion.completeExceptionally(unwrapCompletion(failure));
                        });
        return completion;
    }

    final CompletionStage<Void> dispatchSpotRouteHandler(
            ZLinkBackendReceived received, ParsedPacket packet) {
        return dispatchSpotRouteHandler(received, packet, null);
    }

    final CompletionStage<Void> dispatchSpotRouteHandler(
            ZLinkBackendReceived received, ParsedPacket packet, CompletableFuture<Void> admission) {
        var requestHeader = packet.header();
        SpotPacketHandlerRegistration handler =
                context.handlerCatalog().packetHandler(packet.packetName());
        if (requestHeader == null || handler == null) {
            if (received.requestSeq().isPresent()) {
                host.replySpotRouteDispatchError(
                        received,
                        packet.packetName(),
                        requestHeader,
                        context.spotId(),
                        ZLinkDispatchErrorReason.HANDLER_MISSING,
                        null);
            } else {
                host.reportSpotRouteSendDropped(received, packet.packetName(), context.spotId());
            }
            closeRouteReceived(received);
            return CompletableFuture.completedFuture(null);
        }
        // Shared envelope requests carry content type and application metadata in ChannelHeader.
        String contentType = requestHeader.contentType();
        Map<String, String> metadata = requestHeader.metadata();
        if (received.requestSeq().isPresent()) {
            if (!handler.request()) {
                host.replySpotRouteDispatchError(
                        received,
                        packet.packetName(),
                        requestHeader,
                        context.spotId(),
                        ZLinkDispatchErrorReason.HANDLER_MISSING,
                        null);
                closeRouteReceived(received);
                return CompletableFuture.completedFuture(null);
            }
            Message payloadCopy = Message.from(packet.payload());
            return host.admitApplicationJob(
                            () -> {
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
                                                startSpotHandler();
                                                received.requestProgress().handlerStarted();
                                                return ZLinkFlowContext.propagate(
                                                        host.runWithOutbound(
                                                                        context.dispatchOutbound(),
                                                                        () ->
                                                                                handlerInvoker
                                                                                        .invokeRequest(
                                                                                                handler,
                                                                                                spotSurface,
                                                                                                payloadCopy,
                                                                                                contentType,
                                                                                                metadata,
                                                                                                received
                                                                                                        .receivedAt(),
                                                                                                context
                                                                                                                .handlerInstances()
                                                                                                        ::instance))
                                                                .thenAccept(
                                                                        reply ->
                                                                                replySpotRoutePayload(
                                                                                        received,
                                                                                        requestHeader,
                                                                                        reply)));
                                            } catch (RuntimeException failure) {
                                                return CompletableFuture.failedFuture(failure);
                                            }
                                        });
                            })
                    .whenComplete(
                            (ignored, error) -> {
                                if (error != null) {
                                    host.replySpotRouteDispatchError(
                                            received,
                                            packet.packetName(),
                                            requestHeader,
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
        ZLinkFlowContext.State inboundFlow = ZLinkFlowContext.current();
        var sourceRid = received.routingId().orElse(null);
        String correlationId = requestHeader == null ? null : requestHeader.correlationId();
        releaseRouteParts(received);
        return host.admitApplicationJob(
                        () -> {
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
                                        startSpotHandler();
                                        return host.runWithOutbound(
                                                context.dispatchOutbound(),
                                                () -> {
                                                    CompletionStage<?> handling;
                                                    try {
                                                        handling =
                                                                handlerInvoker.invokePacket(
                                                                        handler,
                                                                        spotSurface,
                                                                        payloadCopy,
                                                                        contentType,
                                                                        metadata,
                                                                        received.receivedAt(),
                                                                        context.handlerInstances()
                                                                                ::instance);
                                                    } catch (RuntimeException decodeFailure) {
                                                        host.recordSpotDrop(
                                                                context.spotId(), "decode_error");
                                                        reportRoutedSendFailure(
                                                                packetName,
                                                                sourceRid,
                                                                correlationId,
                                                                inboundFlow,
                                                                ZLinkDispatchErrorReason
                                                                        .PAYLOAD_DECODE_FAILED,
                                                                decodeFailure);
                                                        throw decodeFailure;
                                                    }
                                                    return handling.handle(
                                                            (ignored, failure) -> {
                                                                if (failure != null) {
                                                                    reportRoutedSendFailure(
                                                                            packetName,
                                                                            sourceRid,
                                                                            correlationId,
                                                                            inboundFlow,
                                                                            ZLinkDispatchErrorReason
                                                                                    .HANDLER_EXCEPTION,
                                                                            failure);
                                                                    throw failure
                                                                                    instanceof
                                                                                    CompletionException
                                                                                            completion
                                                                            ? completion
                                                                            : new CompletionException(
                                                                                    failure);
                                                                }
                                                                return null;
                                                            });
                                                });
                                    },
                                    admission);
                        })
                .whenComplete(
                        (ignored, error) -> {
                            payloadCopy.close();
                            if (error == null) {
                                try (var flowScope =
                                        inboundFlow == null
                                                ? null
                                                : ZLinkFlowContext.enter(inboundFlow)) {
                                    host.traceSpotRouteFlow(
                                            ZLinkMessageFlowOutcome.COMPLETED,
                                            ZLinkDispatchMessageKind.SEND,
                                            packetName,
                                            Optional.empty(),
                                            context.spotId());
                                }
                            }
                        });
    }

    private void reportRoutedSendFailure(
            String packetName,
            RoutingId sourceRid,
            String correlationId,
            ZLinkFlowContext.State flow,
            ZLinkDispatchErrorReason reason,
            Throwable failure) {
        if (!host.dispatchErrorsEnabled()) return;
        try (var ignored = ZLinkFlowContext.enter(flow)) {
            host.reportDispatchError(
                    DispatchFailureReport.of(
                                    ZLinkDispatchErrorSurface.SPOT_ROUTE,
                                    ZLinkDispatchMessageKind.SEND,
                                    reason,
                                    ZLinkDispatchErrorAction.DROP)
                            .packetName(packetName)
                            .spotId(context.spotId())
                            .sourceRid(sourceRid)
                            .correlationId(correlationId)
                            .error(failure));
        }
    }

    final CompletionStage<Optional<Message>> handleRoutedActorPacketParts(
            List<Message> parts, Instant receivedAt) {
        List<Message> packetParts =
                parts.size() == 2
                        ? systems.zlink.framework.runtime.actors.ZLinkActorEntryTransferEnvelope
                                .decode(parts.get(1))
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
            host.actorAdmissions()
                    .traceTransferMarker(
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
                                        packet.payload(),
                                        receivedAt)
                                : host.dispatchMessageFollow(
                                        packet.actorRef(),
                                        packet.header(),
                                        packet.payload(),
                                        packet.acceptedJournalRecord(),
                                        receivedAt)
                        : host.dispatchTransferBacklog(
                                        packet.actorRef(),
                                        packet.header(),
                                        packet.payload(),
                                        packet.acceptedJournalRecord(),
                                        receivedAt)
                                .thenApply(
                                        reply ->
                                                reply.map(
                                                        message ->
                                                                new LocalActorReply(
                                                                        message,
                                                                        packet.header().codec())));
        return dispatched
                .thenCompose(
                        reply ->
                                host.replyTransferredRequestDirect(
                                                packet.actorRef(),
                                                packet.header(),
                                                packet.replyRoute(),
                                                reply)
                                        .thenApply(
                                                completed -> {
                                                    if (packet.replyRoute() != null) {
                                                        return Optional.of(
                                                                ZLinkActorSpotRoutePackets
                                                                        .createHandoffDirectReplyAck());
                                                    }
                                                    if (completed.isEmpty()) {
                                                        return Optional.<Message>empty();
                                                    }
                                                    LocalActorReply actorReply =
                                                            completed.orElseThrow();
                                                    try (Message ignored = actorReply.payload()) {
                                                        return Optional.of(
                                                                ActorPacketFrames.encodeRoutedReply(
                                                                        packet.header(),
                                                                        actorReply));
                                                    }
                                                }))
                .whenComplete((ignored, error) -> packet.close());
    }

    /**
     * Admits one Logical Multicast child. A receive owner offers the permit it holds for this child
     * in the current scope, and the child takes it: a 1:N fanout posts no more jobs than the
     * permits it holds (spec 04 application job queue). A local publication holds none, so the
     * child takes its own host permit before it is posted (spec 04 section 3).
     */
    final Boolean admitTopic(ZLinkBackendTopicMessage received) {
        if (systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                .hasTransferableQueuedOwnership()) {
            return postTopic(received);
        }
        host.admitNewApplicationJob(
                        context, () -> CompletableFuture.completedFuture(postTopic(received)))
                .whenComplete(
                        (posted, failure) -> {
                            if (failure != null) received.parts().forEach(Message::close);
                        });
        return true;
    }

    /** Posts one topic child with the permit the current scope carries for it. */
    boolean postTopic(ZLinkBackendTopicMessage received) {
        dispatchSpotSubscription(received);
        return true;
    }

    final CompletionStage<Void> dispatchSpotSubscription(ZLinkBackendTopicMessage received) {
        try {
            if (received.parts().isEmpty()) {
                host.reportSpotSubscriptionDropped(
                        received.topic(),
                        null,
                        context.spotId(),
                        ZLinkDispatchErrorReason.INVALID_FRAME);
                return CompletableFuture.completedFuture(null);
            }
            ParsedPacket packet;
            try {
                packet = host.parsePacket(received.parts());
            } catch (ZLinkFrameworkException invalidEnvelope) {
                //  Spec 27 §3: a malformed one-way envelope is dropped as
                //  invalid_frame.
                host.reportSpotSubscriptionDropped(
                        received.topic(),
                        null,
                        context.spotId(),
                        ZLinkDispatchErrorReason.INVALID_FRAME);
                return CompletableFuture.completedFuture(null);
            }
            var publishHeader = packet.header();
            String contentType =
                    publishHeader != null ? publishHeader.contentType() : received.contentType();
            Map<String, String> metadata;
            try {
                metadata =
                        publishHeader != null && !publishHeader.metadata().isEmpty()
                                ? publishHeader.metadata()
                                : ZLinkApplicationMetadata.decode(received.applicationMetadata());
            } catch (IllegalArgumentException error) {
                host.reportSpotSubscriptionDropped(
                        received.topic(),
                        null,
                        context.spotId(),
                        ZLinkDispatchErrorReason.INVALID_FRAME);
                return CompletableFuture.completedFuture(null);
            }
            CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
            List<SpotSubscriptionHandlerRegistration> matchingHandlers =
                    context
                            .handlerCatalog()
                            .subscriptionHandlers(received.channelName(), received.topic())
                            .stream()
                            .filter(handler -> handler.packetName().equals(packet.packetName()))
                            .toList();
            if (!matchingHandlers.isEmpty()) {
                ZLinkInboundPayloadOwner payloadOwner =
                        handlerInvoker.payloadOwner(packet.payload(), contentType);
                Object decoded =
                        handlerInvoker.deserializeSubscription(
                                matchingHandlers.get(0), payloadOwner);
                boolean reuseIngressPermit =
                        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                                .current()
                                .isPresent();
                for (int index = 0; index < matchingHandlers.size(); index++) {
                    SpotSubscriptionHandlerRegistration handler = matchingHandlers.get(index);
                    CompletionStage<Void> prior = tail;
                    Supplier<CompletionStage<Void>> job =
                            () ->
                                    appendSpotHandler(
                                            prior,
                                            packet.payload().size(),
                                            () -> {
                                                startSpotHandler();
                                                return host.runWithOutbound(
                                                        context.dispatchOutbound(),
                                                        () ->
                                                                handlerInvoker
                                                                        .invokeSubscriptionDecoded(
                                                                                handler,
                                                                                spotSurface,
                                                                                received
                                                                                        .channelName(),
                                                                                received.topic(),
                                                                                received.routingId()
                                                                                        .map(
                                                                                                Object
                                                                                                        ::toString),
                                                                                decoded,
                                                                                contentType,
                                                                                metadata,
                                                                                context
                                                                                                .handlerInstances()
                                                                                        ::instance));
                                            });
                    tail =
                            reuseIngressPermit && index == 0
                                    ? job.get()
                                    : host.admitNewApplicationJob(context, job);
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

    /** Writes a kind-2 reply with the request's identity and flow fields. */
    private static void replySpotRoutePayload(
            ZLinkBackendReceived received,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header requestHeader,
            Message reply) {
        Message replyHeader =
                systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.encodeHeader(
                        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.reply(
                                requestHeader));
        received.reply(List.of(replyHeader, reply));
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

    final void startSpotHandler() {
        systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobContext
                .beforeFirstApplicationInstruction();
    }
}
