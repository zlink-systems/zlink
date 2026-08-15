package systems.zlink.framework.runtime.actors;
import java.util.Objects;
import java.util.Optional;
import java.util.logging.Logger;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.ZLinkSpotKind;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorAction;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorReason;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchErrorSurface;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkDispatchMessageKind;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowEvent;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkMessageFlowOutcome;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkActorSpotJoinCall implements ZLinkActorJoinCall {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final Logger LOGGER =
        Logger.getLogger(ZLinkActorSpotJoinCall.class.getName());
    private final ZLinkActorRuntime.DefaultActorContext context;
    private final String spotId;
    private final Message request;
    private final Duration timeout;
    private final Services services;
    private final String internalRouteChannel;
    private final RoutingId internalTargetNode;
    private final RoutingId explicitTargetNode;
    private final String explicitRouterChannelId;
    private final boolean entryTarget;
    private AtomicBoolean deferred = new AtomicBoolean();
    private final AtomicBoolean acceptedCallbackDeliveredOnTarget =
        new AtomicBoolean();

    ZLinkActorSpotJoinCall(
        ZLinkActorRuntime.DefaultActorContext context,
        String spotId,
        Message request,
        Duration timeout,
        Services services) {
        this.context = context;
        this.spotId = spotId;
        this.request = request;
        this.timeout = timeout;
        this.services = services;
        this.internalRouteChannel = null;
        this.internalTargetNode = null;
        this.explicitTargetNode = null;
        this.explicitRouterChannelId = null;
        this.entryTarget = false;
    }

    ZLinkActorSpotJoinCall(
        ZLinkActorRuntime.DefaultActorContext context,
        String spotId,
        Message request,
        Duration timeout,
        Services services,
        boolean entryTarget) {
        this.context = context;
        this.spotId = spotId;
        this.request = request;
        this.timeout = timeout;
        this.services = services;
        this.internalRouteChannel = null;
        this.internalTargetNode = null;
        this.explicitTargetNode = null;
        this.explicitRouterChannelId = null;
        this.entryTarget = entryTarget;
    }

    ZLinkActorSpotJoinCall(
        ZLinkActorRuntime.DefaultActorContext context,
        String spotId,
        RoutingId targetNode,
        String routerChannelId,
        Message request,
        Duration timeout,
        Services services,
        boolean entryTarget) {
        this.context = context;
        this.spotId = spotId;
        this.request = request;
        this.timeout = timeout;
        this.services = services;
        this.internalRouteChannel = null;
        this.internalTargetNode = null;
        this.explicitTargetNode = targetNode;
        this.explicitRouterChannelId = routerChannelId;
        this.entryTarget = entryTarget;
    }

    ZLinkActorSpotJoinCall(
        ZLinkActorRuntime.DefaultActorContext context,
        String routeChannel,
        RoutingId targetNode,
        Message request,
        Duration timeout,
        Services services) {
        this.context = context;
        this.spotId = targetNode.toString();
        this.request = request;
        this.timeout = timeout;
        this.services = services;
        this.internalRouteChannel = routeChannel;
        this.internalTargetNode = targetNode;
        this.explicitTargetNode = targetNode;
        this.explicitRouterChannelId = routeChannel;
        this.entryTarget = true;
    }

    @Override
    public ZLinkActorJoinCall timeout(Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            throw new ZLinkConfigurationException("timeout must be positive");
        }
        ZLinkActorSpotJoinCall configured = internalRouteChannel == null
            ? explicitTargetNode == null
                ? new ZLinkActorSpotJoinCall(
                    context, spotId, request, timeout, services, entryTarget)
                : new ZLinkActorSpotJoinCall(
                    context, spotId, explicitTargetNode, explicitRouterChannelId,
                    request, timeout, services, entryTarget)
            : new ZLinkActorSpotJoinCall(
                context, internalRouteChannel, internalTargetNode, request, timeout, services);
        configured.deferred = deferred;
        return configured;
    }

    @Override
    public void defer() {
        if (!deferred.compareAndSet(false, true)) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INVALID_OPERATION,
                "Actor join call was already deferred");
        }
        services.actors().requireDeferredJoinRegistration(context);
        validateTimeout(timeout);
        long timeoutNanos = timeout.toNanos();
        long now = System.nanoTime();
        long deadline = saturatedDeadline(now, timeoutNanos);
        ZLinkActorJoinOperationId operationId = newOperationId();
        if (!context.tryClaimDeferredJoin(deferred)) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.UNAVAILABLE,
                "Actor already has a pending membership transition");
        }
        try {
            ZLinkDeferredActorJoinScope.registerWithActorBarrier(
                services.actors().deferredJoinRuntimeScope(),
                services.actors().deferredJoinIncarnation(context),
                context.actorRef().actorId(),
                request.size(),
                deadline,
                () -> executeDeferred(operationId, deadline),
                operation -> services.actors().submitDeferredJoinBarrier(
                    context.actorRef().actorId(),
                    operation),
                () -> context.releaseDeferredJoin(deferred));
        } catch (RuntimeException error) {
            context.releaseDeferredJoin(deferred);
            throw error;
        }
    }

    CompletionStage<ZLinkActorJoinOutcome> execute() {
        return execute(
            newOperationId(),
            saturatedDeadline(System.nanoTime(), timeout.toNanos()));
    }

    CompletionStage<ZLinkActorJoinOutcome> execute(
        ZLinkActorJoinOperationId operationId) {
        return execute(
            operationId,
            saturatedDeadline(System.nanoTime(), timeout.toNanos()));
    }

    CompletionStage<ZLinkActorJoinOutcome> execute(
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        Objects.requireNonNull(operationId, "operationId");
        rejectSameGateWait();
        Duration remaining = remainingTimeout(deadlineNanos);
        if (remaining == null) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "Actor join deadline elapsed before admission"));
        }
        return executeAfterPredecessor(operationId, deadlineNanos);
    }

    private CompletionStage<ZLinkActorJoinOutcome> executeAfterPredecessor(
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        Duration remaining = remainingTimeout(deadlineNanos);
        if (remaining == null) {
            return CompletableFuture.failedFuture(
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                    "Actor join deadline elapsed at predecessor gate"));
        }
        traceJoinSent();
        Message requestPart = Message.from(request);
        ZLinkSpot<?> localSpot = services.spotResolver().apply(spotId);
        if (localSpot == null
            && (services.routedTransport() != null
                || services.transferTransport() != null)
            && (internalRouteChannel != null || services.remoteAddressResolver() != null)) {
            return manage(joinRemoteRoutedSpot(requestPart, operationId, deadlineNanos)
                .whenComplete((ignored, error) -> requestPart.close())
                .thenCompose(this::decodeCanonicalJoinResult)
                .whenComplete((r, e) -> traceJoinReplyReceived(e)));
        }
        CompletionStage<SpotTransportAddress> target =
            localSpot != null
                ? CompletableFuture.completedFuture(localAddress())
                : resolveRemoteAddress(spotId);
        return manage(target.handle((address, error) -> {
                if (error != null) {
                    requestPart.close();
                    throw new CompletionException(error);
                }
                try {
                    return services.spotNode().joinActor(
                        context.actorRef(),
                        address.targetNodeRid(),
                        spotId,
                        address.spotGeneration(),
                        List.of(requestPart),
                        remaining)
                        .whenComplete((ignored, joinError) ->
                            requestPart.close());
                } catch (RuntimeException dispatchError) {
                    requestPart.close();
                    throw dispatchError;
                }
            })
            .thenCompose(stage -> stage)
            .whenComplete((result, error) -> {
                if (localSpot != null && error != null) {
                    services.actors().cancelLocalJoin(context.actor());
                }
            })
            .thenCompose(result -> completeLocalJoin(localSpot, result)
                .thenCompose(ignored -> applyRemoteActorMigration(result))
                .thenCompose(ignored -> decodeJoinResultAsync(result)))
            .whenComplete((r, e) -> traceJoinReplyReceived(e)));
    }

    private CompletionStage<Void> executeDeferred(
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        Duration remaining = remainingTimeout(deadlineNanos);
        if (remaining == null) {
            return notifyCompletion(new ZLinkActorJoinCompletion.Failed(
                operationId,
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED));
        }
        ZLinkActorSpotJoinCall bounded =
            (ZLinkActorSpotJoinCall) timeout(remaining);
        // A deferred Join is an infrastructure/lifecycle operation. It must
        // not inherit the application execution context of the handler that
        // registered it; otherwise a synchronous barrier activation can make
        // the Join's own same-Actor wait look like a forbidden application
        // wait.
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(null)) {
            return bounded.execute(operationId, deadlineNanos).handle((result, error) -> {
                    if (error != null) {
                        Throwable cause = unwrap(error);
                        ZLinkFrameworkErrorKind kind = cause instanceof ZLinkFrameworkException framework
                            ? framework.kind()
                            : ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
                        //  A deferred Join that ends in `Failed` reports only
                        //  an error kind to the application. Trace the cause on
                        //  the message flow so the failure carries the same
                        //  flow identity as the Join that produced it.
                        traceJoinFailed(kind, cause);
                        return (ZLinkActorJoinCompletion) new ZLinkActorJoinCompletion.Failed(
                            operationId,
                            kind);
                    }
                    if (result instanceof ZLinkActorJoinOutcome.Accepted accepted) {
                        return new ZLinkActorJoinCompletion.Accepted(
                            operationId,
                            accepted.actor(),
                            accepted.reply());
                    }
                    ZLinkActorJoinOutcome.Rejected rejected =
                        (ZLinkActorJoinOutcome.Rejected) result;
                    return new ZLinkActorJoinCompletion.Rejected(
                        operationId,
                        rejected.reply());
                })
                .thenCompose(completion ->
                    completion instanceof ZLinkActorJoinCompletion.Accepted
                        && bounded.acceptedCallbackDeliveredOnTarget.get()
                        ? CompletableFuture.completedFuture(null)
                        : notifyCompletion(completion));
        }
    }

    static Duration remainingTimeout(long deadlineNanos) {
        long remainingNanos = deadlineNanos - System.nanoTime();
        return remainingNanos <= 0
            ? null
            : Duration.ofNanos(remainingNanos);
    }

    static long saturatedDeadline(long now, long timeoutNanos) {
        return timeoutNanos > 0
            && now > Long.MAX_VALUE - timeoutNanos
            ? Long.MAX_VALUE
            : now + timeoutNanos;
    }

    private CompletionStage<Void> notifyCompletion(
        ZLinkActorJoinCompletion completion) {
        String outcomeDetails = completion instanceof ZLinkActorJoinCompletion.Failed failed
            ? " kind=" + failed.kind()
            : "";
        streamTrace("join completion callback-start actor="
            + context.actorRef().actorId()
            + " outcome=" + completion.getClass().getSimpleName()
            + outcomeDetails);
        try {
            CompletionStage<Void> stage =
                context.actor().onJoinCompleted(completion);
            CompletionStage<Void> normalized = stage == null
                ? CompletableFuture.completedFuture(null)
                : stage;
            return normalized.whenComplete((ignored, error) ->
                streamTrace("join completion callback-complete actor="
                    + context.actorRef().actorId()
                    + " outcome=" + completion.getClass().getSimpleName()
                    + " error=" + (error == null ? "none" : error)));
        } catch (RuntimeException error) {
            streamTrace("join completion callback-throw actor="
                + context.actorRef().actorId()
                + " outcome=" + completion.getClass().getSimpleName()
                + " error=" + error);
            return CompletableFuture.failedFuture(error);
        }
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] " + message);
        }
    }

    static ZLinkActorJoinOperationId newOperationId() {
        UUID id;
        do {
            id = UUID.randomUUID();
        } while (id.getMostSignificantBits() == 0L
            && id.getLeastSignificantBits() == 0L);
        return new ZLinkActorJoinOperationId(
            id.getMostSignificantBits(),
            id.getLeastSignificantBits());
    }

    static void validateTimeout(Duration timeout) {
        long millis;
        try {
            millis = timeout.toMillis();
            Duration remainder = timeout.minusMillis(millis);
            if (!remainder.isZero() && !remainder.isNegative()) {
                millis = Math.addExact(millis, 1L);
            }
        } catch (ArithmeticException error) {
            throw new ZLinkConfigurationException(
                "timeout must fit the finite 1..2147483647 ms range");
        }
        if (millis < 1L || millis > Integer.MAX_VALUE) {
            throw new ZLinkConfigurationException(
                "timeout must fit the finite 1..2147483647 ms range");
        }
    }

    private static Throwable unwrap(Throwable error) {
        return error instanceof CompletionException && error.getCause() != null
            ? error.getCause()
            : error;
    }

    private static <T> CompletionStage<T> manage(CompletionStage<T> stage) {
        return ZLinkAsyncSerialQueue.manageCurrent(stage);
    }

    private void rejectSameGateWait() {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectCurrentActorJoinWait(
                context.actorRef().actorId());
    }

    private CompletionStage<Void> completeLocalJoin(
        ZLinkSpot<?> localSpot,
        ZLinkBackendActorJoinResult result) {
        if (localSpot == null || result.result() != ZLinkBackendRequestResult.OK
            || result.joinResultCode() != 0) {
            return CompletableFuture.completedFuture(null);
        }
        return services.actors().completeLocalJoinFromCaller(context.actor());
    }

    private void traceJoinSent() {
        if (services.flow() != null && services.flow().enabled(ZLinkMessageFlowOutcome.SENT)) {
            services.flow().trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.SENT,
                ZLinkDispatchErrorSurface.SPOT_ACTOR,
                ZLinkDispatchMessageKind.ACTOR_REQUEST,
                "JoinSpot", null, null, null, null,
                spotId.toString(), context.actorRef().actorId(), null));
        }
    }

    private void traceJoinFailed(ZLinkFrameworkErrorKind kind, Throwable cause) {
        if (services.flow() == null) {
            return;
        }
        services.flow().traceLazy(
            ZLinkMessageFlowOutcome.ERROR,
            () -> new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.ERROR,
                ZLinkDispatchErrorSurface.SPOT_ACTOR,
                ZLinkDispatchMessageKind.ACTOR_REQUEST,
                "JoinSpot", null, null, null, null,
                spotId == null ? null : spotId.toString(),
                context.actorRef().actorId(), null,
                ZLinkDispatchErrorReason.HANDLER_EXCEPTION,
                ZLinkDispatchErrorAction.REPLY_ERROR,
                kind.name(),
                cause == null ? null : cause.toString()));
    }

    private void traceJoinReplyReceived(Throwable error) {
        if (error == null
            && services.flow() != null
            && services.flow().enabled(ZLinkMessageFlowOutcome.REPLY_RECEIVED)) {
            services.flow().trace(new ZLinkMessageFlowEvent(
                ZLinkMessageFlowOutcome.REPLY_RECEIVED,
                ZLinkDispatchErrorSurface.SPOT_ACTOR,
                ZLinkDispatchMessageKind.RESPONSE,
                "JoinSpot", null, null, null, null,
                spotId.toString(), context.actorRef().actorId(), null));
        }
    }

    private ZLinkActorJoinOutcome decodeJoinResult(ZLinkBackendActorJoinResult result) {
        requireJoinCompleted(result);
        ZLinkActorJoinOutcome decoded = ZLinkActorJoinResults.decode(
            services.serializer(),
            result.joinResultCode(),
            result.actor(),
            context.meshName(),
            result.replyParts());
        if (!entryTarget && decoded instanceof ZLinkActorJoinOutcome.Accepted) {
            String joinedSpotId = effectiveJoinedSpotId(result);
            context.markJoined(result.actor(), joinedSpotId, services.spotResolver().apply(joinedSpotId));
        }
        return decoded;
    }

    private CompletionStage<ZLinkActorJoinOutcome> decodeJoinResultAsync(
        ZLinkBackendActorJoinResult result) {
        ZLinkActorJoinOutcome decoded = decodeJoinResult(result);
        if (decoded instanceof ZLinkActorJoinOutcome.Rejected) {
            return CompletableFuture.completedFuture(decoded);
        }
        return entryTarget
            ? CompletableFuture.completedFuture(decoded)
            : services.locationRenewal().renew(context.actor(), context.joinedSpotId())
                .thenApply(ignored -> decoded);
    }

    private void requireJoinCompleted(ZLinkBackendActorJoinResult result) {
        if (result.result() != ZLinkBackendRequestResult.OK) {
            //  Spec 15-spot-actor:364-375 — a Join Failed.Kind is a runtime
            //  terminal, never NotConfigured.
            throw new ZLinkFrameworkException(
                result.result().toFrameworkErrorKind(),
                "actor spot join failed: " + result.result());
        }
    }

    private CompletionStage<Void> applyRemoteActorMigration(ZLinkBackendActorJoinResult result) {
        if (result.result() != ZLinkBackendRequestResult.OK
            || result.joinResultCode() != 0
            || result.actor() == null
            || context.actorRef() == null
            || result.actor().equals(context.actorRef())) {
            return CompletableFuture.completedFuture(null);
        }
        return context.rebindNativeActor(result.actor(), timeout)
            .thenRun(() -> {
                String joinedSpotId = effectiveJoinedSpotId(result);
                if (entryTarget) {
                    context.markMovedToEntrySpot(
                        result.actor(),
                        new ZLinkActorRuntime.EntrySpotTarget(
                            result.actor().nodeRid(),
                            joinedSpotId));
                } else {
                    context.markJoined(
                        result.actor(), joinedSpotId,
                        services.spotResolver().apply(joinedSpotId));
                }
                services.actors().abandonSourceLocationOwnership(
                    context.actor());
                services.actors().completeRemoteMove(context.actor());
            });
    }

    private String effectiveJoinedSpotId(ZLinkBackendActorJoinResult result) {
        String joinedSpotId = result.joinedSpotId();
        return joinedSpotId == null || joinedSpotId.toString().isBlank()
            ? spotId
            : joinedSpotId;
    }

    private CompletionStage<ZLinkBackendActorJoinResult> joinRemoteRoutedSpot(
        Message requestPart,
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        CompletionStage<SpotTransportAddress> resolved = internalRouteChannel == null
            ? explicitRouterChannelId != null && !explicitRouterChannelId.isBlank()
                ? CompletableFuture.completedFuture(new SpotTransportAddress(
                    explicitRouterChannelId,
                    explicitTargetNode,
                    spotId,
                    0L,
                    0L,
                    ZLinkSpotKind.ENTRY))
                : resolveHandle(services.remoteAddressResolver(), spotId)
            .thenCompose(services.remoteAddressResolver()::resolve)
            .thenApply(address -> address.map(value -> explicitTargetNode == null
                ? value
                : new SpotTransportAddress(
                    value.routerChannelId(),
                    explicitTargetNode,
                    spotId,
                    value.spotGeneration(),
                    value.targetNodeGeneration(),
                    value.authorityOwnerGeneration(),
                    value.ownerLeaseGeneration(),
                    value.spotKind())))
            .thenApply(address -> address.orElseThrow(() ->
                new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.UNAVAILABLE,
                    "SPOT transport address was not found: " + spotId)))
            : resolveRemoteAddress(spotId).thenApply(address -> {
                if (!internalTargetNode.equals(address.targetNodeRid())) {
                    throw new ZLinkConfigurationException(
                        "resolved drain target does not match selected node: "
                            + internalTargetNode);
                }
                return new SpotTransportAddress(
                    internalRouteChannel,
                    address.targetNodeRid(),
                    address.spotId(),
                    address.spotGeneration(),
                    address.targetNodeGeneration(),
                    address.authorityOwnerGeneration(),
                    address.ownerLeaseGeneration(),
                    address.spotKind());
            });
        return resolved.thenCompose(target -> {
                rememberResolvedSpotAuthority(target);
                ZLinkBackendActorRef currentActorRef = context.actorRef();
                String actorType = actorTypeOrEmpty(currentActorRef.actorId());
                String transferId = newRelocationId(operationId).toString();
                List<Message> admissionParts =
                    ZLinkActorSpotRoutePackets.createCanonicalAdmissionRequestParts(
                    transferId,
                    timeout,
                    currentActorRef.actorId(),
                    actorType,
                    currentActorRef,
                    context.entrySpotNodeRid(),
                    context.entrySpotId(),
                    entryRouterChannelId(target),
                    context.boundSessionSourceNodeRid(),
                    context.boundSessionSourceSessionRid(),
                    requestPart,
                    operationId);
                try {
                    return requestTransfer(target, admissionParts)
                        .thenCompose(replyParts -> {
                            try {
                                if (replyParts.isEmpty()) {
                                    return CompletableFuture.failedFuture(
                                        new ZLinkFrameworkException(
                                            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                                            "remote actor Spot admission reply was empty: " + spotId));
                                }
                                ZLinkActorSpotRoutePackets.AdmissionReply admission =
                                    ZLinkActorSpotRoutePackets.decodeAdmissionReply(replyParts.get(0));
                                if (!admission.accepted()) {
                                    return CompletableFuture.completedFuture(
                                        rejectedRemoteJoin(currentActorRef, admission.reply()));
                                }
                                return submitCanonicalRelocation(
                                    target,
                                    transferId,
                                    actorType,
                                    currentActorRef,
                                    admission.reply(),
                                    operationId,
                                    deadlineNanos);
                            } finally {
                                replyParts.forEach(Message::close);
                            }
                        })
                        .whenComplete((ignored, admissionError) ->
                            admissionParts.forEach(Message::close));
                } catch (RuntimeException error) {
                    admissionParts.forEach(Message::close);
                    throw error;
                }
            });
    }

    private CompletionStage<ZLinkBackendActorJoinResult>
        submitCanonicalRelocation(
            SpotTransportAddress address,
            String transferId,
            String actorType,
            ZLinkBackendActorRef sourceActor,
            Message admissionReply,
            ZLinkActorJoinOperationId operationId,
            long deadlineNanos) {
        Duration remaining = remainingTimeout(deadlineNanos);
        byte[] rawReply;
        try {
            rawReply = admissionReply.toByteArray();
        } finally {
            admissionReply.close();
        }
        if (remaining == null) {
            return CompletableFuture.failedFuture(new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
                "Actor join deadline elapsed after admission"));
        }
        ZLinkActorJoinRelocationPort.Goal goal =
            new ZLinkActorJoinRelocationPort.Goal(
                UUID.fromString(transferId),
                operationId,
                sourceActor,
                actorType,
                address.spotId(),
                address.spotGeneration(),
                address.targetNodeRid(),
                address.targetNodeGeneration(),
                address.authorityOwnerGeneration(),
                address.ownerLeaseGeneration(),
                rawReply);
        return services.actors().relocateActorJoin(goal, remaining)
            .thenApply(submission -> {
                acceptedCallbackDeliveredOnTarget.set(true);
                ZLinkBackendActorRef targetActor = submission.targetActor();
                return new ZLinkBackendActorJoinResult(
                    ZLinkBackendRequestResult.OK,
                    0,
                    targetActor,
                    address.spotId(),
                    address.spotGeneration(),
                    0,
                    List.of(Message.from(rawReply)));
            });
    }

    private CompletionStage<ZLinkActorJoinOutcome> decodeCanonicalJoinResult(
        ZLinkBackendActorJoinResult result) {
        requireJoinCompleted(result);
        return CompletableFuture.completedFuture(ZLinkActorJoinResults.decode(
            services.serializer(),
            result.joinResultCode(),
            result.actor(),
            context.meshName(),
            result.replyParts()));
    }

    private static UUID newRelocationId(
        ZLinkActorJoinOperationId operationId) {
        UUID id;
        do {
            id = UUID.randomUUID();
        } while (operationId != null
            && id.getMostSignificantBits() == operationId.high()
            && id.getLeastSignificantBits() == operationId.low());
        return id;
    }

    private void rememberResolvedSpotAuthority(SpotTransportAddress target) {
        if (target.spotGeneration() <= 0
            || target.authorityOwnerGeneration() <= 0
            || target.ownerLeaseGeneration() <= 0) {
            return;
        }
        services.spotNode().entrySpot().rememberSpotAuthority(
            target.targetNodeRid(),
            target.spotId(),
            target.spotGeneration(),
            target.authorityOwnerGeneration(),
            target.ownerLeaseGeneration());
    }

    private CompletionStage<List<Message>> requestTransfer(
        SpotTransportAddress address,
        List<Message> parts) {
        if (services.transferTransport() != null) {
            return services.transferTransport().request(
                address,
                parts,
                timeout,
                internalRouteChannel != null);
        }
        if (internalRouteChannel == null) {
            Message packetName = Message.from(parts.getFirst());
            Message envelope = ZLinkActorEntryTransferEnvelope.encode(parts);
            List<Message> wireParts = List.of(packetName, envelope);
            try {
                return services.routedTransport().requestToSpotViaRouterChannel(
                        address.routerChannelId(),
                        address.targetNodeRid(),
                        address.spotId(),
                        address.spotGeneration(),
                        wireParts,
                        timeout)
                    .whenComplete((ignored, error) ->
                        wireParts.forEach(Message::close));
            } catch (RuntimeException error) {
                wireParts.forEach(Message::close);
                throw error;
            }
        }
        Message envelope = ZLinkActorEntryTransferEnvelope.encode(parts);
        return services.routedTransport().requestInternalToNode(
                address.routerChannelId(),
                address.targetNodeRid(),
                ZLinkActorEntryTransferEnvelope.PACKET_NAME,
                envelope,
                timeout)
            .thenApply(reply -> {
                try {
                    return ZLinkActorEntryTransferEnvelope.decode(reply);
                } finally {
                    reply.close();
                }
            })
            .whenComplete((ignored, error) -> envelope.close());
    }

    private String entryRouterChannelId(SpotTransportAddress target) {
        String existing = context.entryRouterChannelId();
        return existing == null || existing.isBlank()
            ? target.routerChannelId()
            : existing;
    }

    private ZLinkBackendActorJoinResult rejectedRemoteJoin(
        ZLinkBackendActorRef currentActorRef,
        Message reply) {
        try (reply) {
            return new ZLinkBackendActorJoinResult(
                ZLinkBackendRequestResult.OK,
                1,
                currentActorRef,
                spotId,
                currentActorRef.generation(),
                0,
                List.of(Message.from(reply)));
        }
    }

    private String actorTypeOrEmpty(String actorId) {
        String actorType = services.actorTypes().apply(actorId);
        return actorType == null ? "" : actorType;
    }

    private CompletionStage<SpotTransportAddress> resolveRemoteAddress(String spotId) {
        if (services.remoteAddressResolver() == null) {
            return CompletableFuture.completedFuture(localAddress());
        }
        return resolveHandle(services.remoteAddressResolver(), spotId)
            .thenCompose(services.remoteAddressResolver()::resolve)
            .thenApply(address -> {
                if (address.isEmpty()
                    || address.get().targetNodeRid() == null
                    || address.get().spotGeneration() <= 0) {
                    throw new ZLinkConfigurationException(
                        "SPOT remote address resolver returned an incomplete owner snapshot: "
                            + spotId);
                }
                return address.get();
            });
    }

    private SpotTransportAddress localAddress() {
        return new SpotTransportAddress(
            "",
            context.actorRef().nodeRid(),
            spotId,
            0L,
            0L,
            ZLinkSpotKind.USER);
    }

    private static CompletionStage<SpotHandle> resolveHandle(
        SpotTransportAddressResolver resolver,
        String spotId) {
        if (!(resolver instanceof SpotHandleResolver handles)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "SPOT transport resolver does not provide opaque handles"));
        }
        return handles.resolveSpotHandle(spotId).thenApply(handle -> handle.orElseThrow(() ->
            new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                "SPOT handle was not found: " + spotId)));
    }

    @FunctionalInterface
    interface ActorJoinedLocationRenewal {
        CompletionStage<Void> renew(ZLinkActor actor, String spotId);
    }

    @FunctionalInterface
    interface TransferTransport {
        CompletionStage<List<Message>> request(
            SpotTransportAddress address,
            List<Message> parts,
            Duration timeout,
            boolean internalRoute);
    }

    record Services(
        ZLinkInternalSpotNode spotNode,
        Function<String, ZLinkSpot<?>> spotResolver,
        SpotTransportAddressResolver remoteAddressResolver,
        ZLinkChannelRuntime routedTransport,
        TransferTransport transferTransport,
        Function<String, String> actorTypes,
        ZLinkMessageSerializer serializer,
        ZLinkMessageFlowTracer flow,
        ZLinkActorRuntime actors,
        ActorJoinedLocationRenewal locationRenewal) {
    }
}
