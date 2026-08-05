package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicReference;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.Function;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.binding.spot.ActorRef;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferId;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferPrepare;
import systems.zlink.framework.runtime.internal.binding.spot.ActorTransferRole;
import systems.zlink.framework.runtime.internal.binding.spot.PrepareActorTransferResult;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.configuration.ZLinkDispatchErrorSurface;
import systems.zlink.framework.configuration.ZLinkDispatchMessageKind;
import systems.zlink.framework.configuration.ZLinkMessageFlowEvent;
import systems.zlink.framework.configuration.ZLinkMessageFlowOutcome;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorJoinResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddress;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceMessageFollowWireCodec;
import systems.zlink.framework.spots.SpotHandle;
import systems.zlink.framework.spots.ZLinkSpot;

final class ZLinkActorSpotJoinCall implements ZLinkActorJoinCall {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final java.util.logging.Logger LOGGER =
        java.util.logging.Logger.getLogger(ZLinkActorSpotJoinCall.class.getName());
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
    private final AtomicReference<SpotTransportAddress> messageFollowAddress =
        new AtomicReference<>();
    private final AtomicBoolean messageFollowInstalled = new AtomicBoolean();
    private final AtomicBoolean acceptedCompletionDeliveredOnTarget =
        new AtomicBoolean();
    private final AtomicReference<ZLinkDeferredJoinAcceptedRecovery.Manifest>
        acceptedCompletionManifest = new AtomicReference<>();
    private final AtomicReference<CompletionStage<Void>>
        deferredSourceCleanup = new AtomicReference<>();

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
            null,
            saturatedDeadline(System.nanoTime(), timeout.toNanos()));
    }

    private CompletionStage<ZLinkActorJoinOutcome> execute(
        ZLinkActorJoinOperationId operationId) {
        return execute(
            operationId,
            saturatedDeadline(System.nanoTime(), timeout.toNanos()));
    }

    private CompletionStage<ZLinkActorJoinOutcome> execute(
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        rejectSameGateWait();
        traceJoinSent();
        Message requestPart = Message.from(request);
        ZLinkSpot<?> localSpot = services.spotResolver().apply(spotId);
        if (localSpot == null && services.routedTransport() != null
            && (internalRouteChannel != null || services.remoteAddressResolver() != null)) {
            return manage(joinRemoteRoutedSpot(requestPart, operationId, deadlineNanos)
                .whenComplete((ignored, error) -> requestPart.close())
                .thenCompose(result -> applyCoreRemoteActorMigration(result)
                    .thenCompose(ignored -> decodeJoinResultAsync(result)))
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
                        timeout)
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
                        && bounded.acceptedCompletionDeliveredOnTarget.get()
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
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(stage);
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
            throw new ZLinkConfigurationException(
                "actor spot join failed: " + result.result());
        }
    }

    private CompletionStage<Void> applyRemoteActorMigration(ZLinkBackendActorJoinResult result) {
        return applyRemoteActorMigration(result, false, false);
    }

    private CompletionStage<Void> applyCoreRemoteActorMigration(
        ZLinkBackendActorJoinResult result) {
        // The source-bound session remains attached to the Message Follow proxy.
        // Framework routing resolves the relocated Spot for subsequent relay.
        return applyRemoteActorMigration(result, true, true);
    }

    private CompletionStage<Void> applyRemoteActorMigration(
        ZLinkBackendActorJoinResult result,
        boolean sessionAlreadyRebound,
        boolean retainMessageFollowSource) {
        if (result.result() != ZLinkBackendRequestResult.OK
            || result.joinResultCode() != 0
            || result.actor() == null
            || context.actorRef() == null
            || result.actor().equals(context.actorRef())) {
            return CompletableFuture.completedFuture(null);
        }
        ZLinkBackendActorRef sourceActorRef = context.actorRef();
        CompletionStage<Void> deferredCleanup = deferredSourceCleanup.get();
        boolean sourceCleanupWasStarted = deferredCleanup != null;
        CompletionStage<Void> rebound = sourceCleanupWasStarted
            ? deferredCleanup
            : sessionAlreadyRebound
                ? CompletableFuture.completedFuture(null)
                : context.rebindNativeActor(result.actor(), timeout);
        Supplier<CompletionStage<Void>> cleanup = () -> rebound
            .thenCompose(ignored -> {
                CompletionStage<
                    java.util.Optional<ZLinkStoreLocationResolvers.ActorRoute>>
                    targetRoute = retainMessageFollowSource
                            && !sourceCleanupWasStarted
                        ? services.actors().resolveMessageFollowTargetRoute(
                            result.actor(),
                            java.util.Objects.requireNonNull(
                                messageFollowAddress.get(),
                                "committed Message Follow address"))
                        : CompletableFuture.completedFuture(
                            java.util.Optional.empty());
                return targetRoute.thenCompose(route -> {
                    if (requiresMessageFollowTargetRoute(
                        retainMessageFollowSource,
                        sourceCleanupWasStarted,
                        route.isPresent())) {
                        return CompletableFuture.failedFuture(
                            new ZLinkConfigurationException(
                                "committed Message Follow target route was unavailable: "
                                    + result.actor().actorId()));
                    }
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
                    if (!sourceCleanupWasStarted) {
                        services.actors().abandonSourceLocationOwnership(
                            context.actor());
                        services.actors().completeRemoteMove(context.actor());
                    }
                    if (retainMessageFollowSource
                        && !sourceCleanupWasStarted
                        && messageFollowInstalled.compareAndSet(false, true)) {
                        services.actors().retainMessageFollowSource(
                            context.actor(), sourceActorRef, result.actor(),
                            java.util.Objects.requireNonNull(
                                messageFollowAddress.get(),
                                "committed Message Follow address"),
                            toMessageFollowActorRoute(route.orElseThrow()));
                    }
                    return services.actors()
                        .completeDeferredJoinAcceptedSourceCleanup(
                            acceptedCompletionManifest.get(),
                            result.actor());
                });
            });
        if (retainMessageFollowSource
            && !deferred.get()
            && services.actors().isActorDispatchActive(context.actor())) {
            // A synchronous Join may complete inside the Actor turn that
            // started it, so source cleanup must follow that turn. A deferred
            // Join completes after its starting turn has returned. Queuing its
            // cleanup behind whichever later Actor request happens to be active
            // can deadlock that request while it waits for the move to finish.
            services.actors().continueAfterActorDispatch(context.actor(), cleanup);
            return CompletableFuture.completedFuture(null);
        }
        return cleanup.get();
    }

    private String effectiveJoinedSpotId(ZLinkBackendActorJoinResult result) {
        String joinedSpotId = result.joinedSpotId();
        return joinedSpotId == null || joinedSpotId.toString().isBlank()
            ? spotId
            : joinedSpotId;
    }

    static boolean requiresMessageFollowTargetRoute(
        boolean retainMessageFollowSource,
        boolean sourceCleanupWasStarted,
        boolean routePresent) {
        return retainMessageFollowSource
            && !sourceCleanupWasStarted
            && !routePresent;
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
                    systems.zlink.framework.spots.ZLinkSpotKind.ENTRY))
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
                new ZLinkConfigurationException("SPOT transport address was not found: " + spotId)))
            : CompletableFuture.completedFuture(new SpotTransportAddress(
                internalRouteChannel,
                internalTargetNode,
                internalTargetNode.toString(),
                0L,
                0L,
                systems.zlink.framework.spots.ZLinkSpotKind.ENTRY));
        return resolved.thenCompose(target -> {
                messageFollowAddress.set(target);
                ZLinkBackendActorRef currentActorRef = context.actorRef();
                String actorType = actorTypeOrEmpty(currentActorRef.actorId());
                String transferId = UUID.randomUUID().toString();
                List<Message> admissionParts = ZLinkActorSpotRoutePackets.createAdmissionRequestParts(
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
                    requestPart);
                try {
                    return requestTransfer(target, admissionParts)
                        .thenCompose(replyParts -> {
                            try {
                                if (replyParts.isEmpty()) {
                                    return CompletableFuture.failedFuture(
                                        new ZLinkConfigurationException(
                                            "remote actor Spot admission reply was empty: " + spotId));
                                }
                                ZLinkActorSpotRoutePackets.AdmissionReply admission =
                                    ZLinkActorSpotRoutePackets.decodeAdmissionReply(replyParts.get(0));
                                if (!admission.accepted()) {
                                    return CompletableFuture.completedFuture(
                                        rejectedRemoteJoin(currentActorRef, admission.reply()));
                                }
                                return commitRemoteTransfer(
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

    private CompletionStage<ZLinkBackendActorJoinResult> commitRemoteTransfer(
        SpotTransportAddress address,
        String transferId,
        String actorType,
        ZLinkBackendActorRef currentActorRef,
        Message admissionReply,
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        ZLinkActor actor = context.actor();
        byte[] admissionReplyBytes = admissionReply.toByteArray();
        AtomicBoolean sourceLeft = new AtomicBoolean();
        AtomicBoolean commitRequestStarted = new AtomicBoolean();
        AtomicBoolean sourceCoreCommitted = new AtomicBoolean();
        AtomicReference<List<ZLinkActorHandoffPacket>> committedBacklog =
            new AtomicReference<>(List.of());
        AtomicReference<List<Message>> commitRetryTemplate =
            new AtomicReference<>(List.of());
        UUID coreId = UUID.fromString(transferId);
        long membershipEpoch =
            services.spotNode().actorMembershipEpoch(currentActorRef.actorId());
        PrepareActorTransferResult prepared;
        try {
            prepared = services.spotNode().prepareActorTransfer(
                new ActorTransferPrepare(
                    ActorTransferRole.SOURCE,
                    new ActorTransferId(
                        coreId.getMostSignificantBits(),
                        coreId.getLeastSignificantBits()),
                    new ActorRef(
                        currentActorRef.nodeRid(),
                        currentActorRef.actorId(),
                        currentActorRef.generation()),
                    membershipEpoch,
                    address.targetNodeRid(),
                    0L,
                    0L,
                    0L),
                timeout);
        } catch (UnsupportedOperationException unavailable) {
            prepared = null;
        }
        PrepareActorTransferResult corePrepared = prepared;
        if (corePrepared != null) {
            services.actors().traceActorTransferMarker(
                "core_source_prepared",
                currentActorRef.actorId(),
                Long.toUnsignedString(corePrepared.result().transferId().high())
                    + ":" + Long.toUnsignedString(corePrepared.result().transferId().low())
                    + ":" + Long.toUnsignedString(corePrepared.result().finalSequence()));
        }
        return services.actors().directJoinSessionRouteCommand(
                actor,
                currentActorRef,
                address.targetNodeRid(),
                coreId)
            .thenCompose(sessionRouteCommand44 ->
                services.actors().beginRemoteMove(actor)
            .thenCompose(ignored -> services.actors().transferOut(actor))
            .thenApply(transfer -> {
                    List<ZLinkActorHandoffPacket> backlog =
                        services.actors().takeRemoteMoveBacklog(actor);
                    committedBacklog.set(backlog);
                    return new TransferCommit(transfer, backlog);
                })
            .thenCompose(commit -> {
                Message transferState = Message.from(
                    commit.transfer().state().toEncodedPayload(services.serializer()).bytes());
                services.actors().traceActorTransferMarker(
                    "manifest_prepare_start", actor.context().actorId(), transferId);
                CompletionStage<ZLinkDeferredJoinAcceptedRecovery.Manifest>
                    completionManifest = operationId == null
                        ? CompletableFuture.completedFuture(null)
                        : services.actors().prepareDeferredJoinRelocation(
                            coreId,
                            operationId,
                            currentActorRef,
                            actorType,
                            address.spotId(),
                            address.targetNodeRid(),
                            commit.transfer().adapterKey() != null,
                            transferState.toByteArray(),
                            canonicalJournal(commit.backlog()),
                            admissionReply.toByteArray(),
                            sessionRouteCommand44);
                return completionManifest.thenCompose(manifest -> {
                    acceptedCompletionManifest.set(manifest);
                    services.actors().traceActorTransferMarker(
                        "manifest_prepare_result",
                        actor.context().actorId(),
                        manifest == null ? "none" : transferId);
                    CompletionStage<Void> sourceCleanup = manifest == null
                        ? CompletableFuture.completedFuture(null)
                        : beginDeferredSourceCleanup(
                            address,
                            currentActorRef,
                            manifest,
                            sourceLeft,
                            transferId);
                    if (manifest != null) {
                        deferredSourceCleanup.set(sourceCleanup);
                    }
                    List<Message> commitParts =
                        ZLinkActorSpotRoutePackets.createCommitRequestParts(
                            transferId,
                            timeout,
                            currentActorRef.actorId(),
                            actorType,
                            currentActorRef,
                            context.entrySpotNodeRid(),
                            context.entrySpotId(),
                            entryRouterChannelId(address),
                            context.boundSessionSourceNodeRid(),
                            context.boundSessionSourceSessionRid(),
                            commit.transfer().adapterKey(),
                            transferState,
                            commit.backlog(),
                            new ZLinkActorSpotRoutePackets.CoreTransfer(
                                corePrepared != null,
                                corePrepared == null ? 0L
                                    : corePrepared.result().transferId().high(),
                                corePrepared == null ? 0L
                                    : corePrepared.result().transferId().low(),
                                membershipEpoch,
                                corePrepared == null ? 0L
                                    : corePrepared.result().finalSequence(),
                                corePrepared == null ? 0L
                                    : corePrepared.result().reserveMessageCount(),
                                corePrepared == null ? 0L
                                    : corePrepared.result().reserveByteCount()),
                            manifest,
                            sessionRouteCommand44);
                    commitRetryTemplate.set(commitParts.stream()
                        .map(Message::from)
                        .toList());
                    services.actors().traceActorTransferMarker(
                        "commit_submit", actor.context().actorId(), transferId);
                    CompletionStage<List<Message>> targetReply;
                    try {
                        commitRequestStarted.set(true);
                        targetReply = requestTransfer(address, commitParts);
                        return sourceCleanup
                            .thenCompose(ignored -> targetReply)
                            .thenCompose(reply -> {
                                notifySourceAfterTargetCommit(
                                    actor,
                                    sourceLeft,
                                    transferId);
                                services.actors().traceActorTransferMarker(
                                    "commit_reply_received",
                                    actor.context().actorId(),
                                    transferId);
                                if (corePrepared != null) {
                                    commitCoreTransfer(
                                        corePrepared,
                                        membershipEpoch,
                                        sourceCoreCommitted);
                                }
                                return installMessageFollowSource(
                                        address, currentActorRef, reply)
                                    .thenCompose(ignored -> forwardLateBacklog(
                                        address, reply, commit.backlog()));
                            })
                            .whenComplete((ignored, commitError) ->
                                commitParts.forEach(Message::close));
                    } catch (RuntimeException error) {
                        commitParts.forEach(Message::close);
                        throw error;
                    }
                }).whenComplete((ignored, error) -> transferState.close());
            })
            .thenApply(commitReply -> decodeCommitReply(
                commitReply.parts(), admissionReply, commitReply.backlog()))
            .thenApply(result -> {
                if (operationId != null) {
                    acceptedCompletionDeliveredOnTarget.set(true);
                }
                return result;
            })
            .exceptionallyCompose(error -> {
                Throwable cause = unwrap(error);
                if (!sourceLeft.get() && !commitRequestStarted.get()) {
                    admissionReply.close();
                    abortCoreTransfer(corePrepared);
                    CompletionStage<Void> restored;
                    try {
                        restored = services.actors().restoreRemoteMoveBacklog(
                            actor,
                            committedBacklog.get());
                    } catch (RuntimeException restoreStartError) {
                        cause.addSuppressed(restoreStartError);
                        return CompletableFuture.failedFuture(cause);
                    }
                    return restored.handle((ignored, restoreError) -> {
                        if (restoreError != null) {
                            cause.addSuppressed(unwrap(restoreError));
                        }
                        throw new CompletionException(cause);
                    });
                }
                CompletionStage<ZLinkBackendActorJoinResult> retryResult;
                try {
                    retryResult = retryCommitReplyAfterLoss(
                        address,
                        commitRetryTemplate.get(),
                        deadlineNanos)
                        .thenCompose(replyParts ->
                            applyRecoveredCommitReply(
                                address,
                                currentActorRef,
                                new ZLinkBackendActorRef(
                                    address.targetNodeRid(),
                                    currentActorRef.actorId(),
                                    currentActorRef.generation()),
                                replyParts,
                                committedBacklog.get(),
                                admissionReply,
                                actor,
                                sourceLeft,
                                transferId,
                                corePrepared,
                                membershipEpoch,
                                sourceCoreCommitted,
                                operationId));
                } catch (RuntimeException retryStartError) {
                    retryResult = CompletableFuture.failedFuture(retryStartError);
                }
                // If the original request never reached the target, the
                // retry can still consume its pending admission. If the
                // target already committed, its commit registry returns the
                // exact terminal without repeating lifecycle or replay.
                return retryResult.exceptionallyCompose(retryError ->
                    recoverCommittedByLookup(
                        address,
                        currentActorRef,
                        admissionReply,
                        admissionReplyBytes,
                        actor,
                        sourceLeft,
                        transferId,
                        corePrepared,
                        membershipEpoch,
                        sourceCoreCommitted,
                        operationId,
                        committedBacklog,
                        cause,
                        deadlineNanos));
            }))
            .whenComplete((ignored, error) ->
                commitRetryTemplate.get().forEach(Message::close));
    }

    private void notifySourceAfterTargetCommit(
        ZLinkActor actor,
        AtomicBoolean sourceLeft,
        String transferId) {
        if (sourceLeft.compareAndSet(false, true)) {
            services.actors().notifySourceForCoreRemoteMove(actor);
            services.actors().traceActorTransferMarker(
                "source_leave_dispatched", actor.context().actorId(), transferId);
        }
    }

    private CompletionStage<Void> beginDeferredSourceCleanup(
        SpotTransportAddress address,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkDeferredJoinAcceptedRecovery.Manifest manifest,
        AtomicBoolean sourceLeft,
        String transferId) {
        ZLinkBackendActorRef targetActorRef = new ZLinkBackendActorRef(
            address.targetNodeRid(),
            sourceActorRef.actorId(),
            sourceActorRef.generation());
        return services.actors()
            .awaitDeferredJoinTargetCompletion(
                manifest,
                targetActorRef,
                timeout)
            .thenRun(() -> notifySourceAfterTargetCommit(
                context.actor(),
                sourceLeft,
                transferId))
            .thenCompose(ignored ->
                ZLinkActorRetryScheduler.retryRouteUntil(
                    timeout,
                    () -> services.actors()
                        .resolveMessageFollowTargetRoute(
                            targetActorRef,
                            address)
                        .thenCompose(route -> route.isPresent()
                            ? CompletableFuture.completedFuture(route)
                            : CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "committed Message Follow target route is not ready"))),
                    ignoredError -> true))
            .thenCompose(route -> {
                if (messageFollowInstalled.compareAndSet(false, true)) {
                    services.actors().retainMessageFollowSource(
                        context.actor(),
                        sourceActorRef,
                        targetActorRef,
                        address,
                        toMessageFollowActorRoute(route.orElseThrow()));
                }
                services.actors().abandonSourceLocationOwnership(
                    context.actor());
                services.actors().completeRemoteMove(context.actor());
                return services.actors()
                    .markDeferredJoinAcceptedSourceCleanup(
                        manifest,
                        targetActorRef);
            });
    }

    private CompletionStage<Void> installMessageFollowSource(
        SpotTransportAddress address,
        ZLinkBackendActorRef sourceActorRef,
        List<Message> commitReplyParts) {
        if (commitReplyParts.isEmpty()) {
            throw new ZLinkConfigurationException(
                "remote actor Spot commit reply was empty: " + spotId);
        }
        ZLinkActorSpotRoutePackets.JoinReply reply =
            ZLinkActorSpotRoutePackets.decodeJoinReply(commitReplyParts.get(0));
        try {
            return installMessageFollowSource(address, sourceActorRef, reply.actorRef());
        } finally {
            reply.reply().close();
        }
    }

    private CompletionStage<Void> installMessageFollowSource(
        SpotTransportAddress address,
        ZLinkBackendActorRef sourceActorRef,
        ZLinkBackendActorRef targetActorRef) {
        return services.actors()
            .resolveMessageFollowTargetRoute(targetActorRef, address)
            .thenCompose(route -> {
                if (route.isEmpty()) {
                    return CompletableFuture.failedFuture(
                        new ZLinkConfigurationException(
                            "committed Message Follow target route was unavailable: "
                                + targetActorRef.actorId()));
                }
                if (messageFollowInstalled.compareAndSet(false, true)) {
                    services.actors().retainMessageFollowSource(
                        context.actor(), sourceActorRef, targetActorRef, address,
                        toMessageFollowActorRoute(route.orElseThrow()));
                }
                return CompletableFuture.completedFuture(null);
            });
    }

    private static ZLinkServiceMessageFollowWireCodec.ActorRoute
        toMessageFollowActorRoute(ZLinkStoreLocationResolvers.ActorRoute route) {
        return new ZLinkServiceMessageFollowWireCodec.ActorRoute(
            route.actorRef().actorId(),
            route.actorRef().objectGeneration(),
            route.nodeRid(),
            route.targetNodeGeneration(),
            route.authorityOwnerGeneration(),
            route.ownerLeaseGeneration());
    }

    private static List<
        systems.zlink.framework.execution.ZLinkAsyncSerialQueue.QueuedRecord>
        canonicalJournal(List<ZLinkActorHandoffPacket> backlog) {
        return backlog.stream()
            .map(packet ->
                new systems.zlink.framework.execution.ZLinkAsyncSerialQueue
                    .QueuedRecord(
                        packet.arrivalIndex(),
                        systems.zlink.framework.runtime.streams
                            .ZLinkStreamFrameCodec.encode(
                                packet.header(),
                                packet.payload().toByteArray())))
            .toList();
    }

    private void abortCoreTransfer(PrepareActorTransferResult prepared) {
        if (prepared == null) {
            return;
        }
        try {
            services.spotNode().abortActorTransfer(prepared.token());
        } catch (RuntimeException ignored) {
            // A target that already activated cannot be rolled back through
            // the source token; authority reconciliation owns that terminal case.
        }
    }

    private void commitCoreTransfer(
        PrepareActorTransferResult prepared,
        long membershipEpoch,
        AtomicBoolean committed) {
        if (committed.compareAndSet(false, true)) {
            try {
                services.spotNode().commitActorTransfer(
                    prepared.token(), membershipEpoch + 1);
            } catch (RuntimeException error) {
                committed.set(false);
                throw error;
            }
        }
    }

    private CompletionStage<List<Message>> requestTransfer(
        SpotTransportAddress address,
        List<Message> parts) {
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

    private CompletionStage<CommitReply> forwardLateBacklog(
        SpotTransportAddress address,
        List<Message> commitReplyParts,
        List<ZLinkActorHandoffPacket> committedBacklog) {
        if (commitReplyParts.isEmpty()) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "remote actor Spot commit reply was empty: " + spotId));
        }
        ZLinkActorSpotRoutePackets.JoinReply joinReply =
            ZLinkActorSpotRoutePackets.decodeJoinReply(commitReplyParts.get(0));
        List<ZLinkActorHandoffPacket> lateBacklog =
            services.actors().finishRemoteMoveBacklog(context.actor());
        CompletionStage<Void> tail = forwardHandoffPackets(
            address, joinReply.actorRef(), lateBacklog);
        joinReply.reply().close();
        return tail.thenApply(ignored -> new CommitReply(commitReplyParts, committedBacklog));
    }

    private CompletionStage<Void> recoverCommittedBacklogAfterReplyLoss(
        SpotTransportAddress address,
        ZLinkBackendActorRef targetActorRef,
        List<ZLinkActorHandoffPacket> committedBacklog) {
        List<ZLinkActorHandoffPacket> lateBacklog =
            services.actors().finishRemoteMoveBacklog(context.actor());
        if (committedBacklog.isEmpty() && lateBacklog.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        List<ZLinkActorHandoffPacket> packets =
            new java.util.ArrayList<>(
                committedBacklog.size() + lateBacklog.size());
        packets.addAll(committedBacklog);
        packets.addAll(lateBacklog);
        packets.sort(java.util.Comparator.comparingLong(
            ZLinkActorHandoffPacket::arrivalIndex));
        // The target's accepted-journal fence makes this replay idempotent if
        // the original commit reached the target but its reply was lost.
        return forwardHandoffPackets(address, targetActorRef, packets);
    }

    private CompletionStage<List<Message>> retryCommitReplyAfterLoss(
        SpotTransportAddress address,
        List<Message> template,
        long deadlineNanos) {
        Duration remaining = remainingTimeout(deadlineNanos);
        if (template.isEmpty() || remaining == null) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "remote Actor commit retry window is unavailable"));
        }
        return ZLinkActorRetryScheduler.retryRouteUntil(
            remaining,
            () -> {
                List<Message> attempt = template.stream()
                    .map(Message::from)
                    .toList();
                try {
                    return requestTransfer(address, attempt)
                        .whenComplete((ignored, error) ->
                            attempt.forEach(Message::close));
                } catch (RuntimeException error) {
                    attempt.forEach(Message::close);
                    throw error;
                }
            },
            ZLinkActorSubmitFaults::retryableSessionActorBindFailure);
    }

    private CompletionStage<ZLinkBackendActorJoinResult>
        applyRecoveredCommitReply(
            SpotTransportAddress address,
            ZLinkBackendActorRef currentActorRef,
            ZLinkBackendActorRef targetActorRef,
            List<Message> replyParts,
            List<ZLinkActorHandoffPacket> committedBacklog,
            Message admissionReply,
            ZLinkActor actor,
            AtomicBoolean sourceLeft,
            String transferId,
            PrepareActorTransferResult corePrepared,
            long membershipEpoch,
            AtomicBoolean sourceCoreCommitted,
            ZLinkActorJoinOperationId operationId) {
        try {
            notifySourceAfterTargetCommit(actor, sourceLeft, transferId);
            if (corePrepared != null) {
                commitCoreTransfer(
                    corePrepared,
                    membershipEpoch,
                    sourceCoreCommitted);
            }
            return installMessageFollowSource(
                    address, currentActorRef, replyParts)
                .thenCompose(ignored -> forwardLateBacklog(
                    address, replyParts, committedBacklog))
                .thenApply(commitReply -> {
                    if (operationId != null) {
                        acceptedCompletionDeliveredOnTarget.set(true);
                    }
                    return decodeCommitReply(
                        commitReply.parts(),
                        admissionReply,
                        commitReply.backlog());
                })
                .whenComplete((ignored, error) -> {
                    if (error != null) {
                        replyParts.forEach(Message::close);
                    }
                });
        } catch (RuntimeException error) {
            replyParts.forEach(Message::close);
            return CompletableFuture.failedFuture(error);
        }
    }

    private CompletionStage<ZLinkBackendActorJoinResult>
        recoverCommittedByLookup(
            SpotTransportAddress address,
            ZLinkBackendActorRef currentActorRef,
            Message admissionReply,
            byte[] admissionReplyBytes,
            ZLinkActor actor,
            AtomicBoolean sourceLeft,
            String transferId,
            PrepareActorTransferResult corePrepared,
            long membershipEpoch,
            AtomicBoolean sourceCoreCommitted,
            ZLinkActorJoinOperationId operationId,
            AtomicReference<List<ZLinkActorHandoffPacket>> committedBacklog,
            Throwable cause,
            long deadlineNanos) {
        Duration remaining = remainingTimeout(deadlineNanos);
        CompletionStage<java.util.Optional<systems.zlink.framework.actors.ActorRef>>
            committedStage;
        if (remaining == null) {
            committedStage = CompletableFuture.completedFuture(
                java.util.Optional.empty());
        } else {
            committedStage = ZLinkActorRetryScheduler.retryRouteUntilPresent(
                remaining,
                () -> services.actors().findCommittedRemoteActor(
                    currentActorRef.actorId(),
                    address.targetNodeRid(),
                    currentActorRef.generation()));
        }
        return committedStage.thenCompose(committed -> {
            if (committed.isEmpty()) {
                admissionReply.close();
                abortCoreTransfer(corePrepared);
                failPackets(committedBacklog.get(), cause);
                services.actors().failRemoteMove(actor, cause);
                return CompletableFuture.failedFuture(cause);
            }
            notifySourceAfterTargetCommit(actor, sourceLeft, transferId);
            if (corePrepared != null) {
                commitCoreTransfer(
                    corePrepared,
                    membershipEpoch,
                    sourceCoreCommitted);
            }
            ZLinkBackendActorRef committedActorRef =
                new ZLinkBackendActorRef(
                    address.targetNodeRid(),
                    currentActorRef.actorId(),
                    currentActorRef.generation());
            return recoverCommittedBacklogAfterReplyLoss(
                    address,
                    committedActorRef,
                    committedBacklog.get())
                .thenCompose(ignored -> installMessageFollowSource(
                    address, currentActorRef, committedActorRef))
                .thenApply(ignored -> {
                    if (operationId != null) {
                        acceptedCompletionDeliveredOnTarget.set(true);
                    }
                    admissionReply.close();
                    return new ZLinkBackendActorJoinResult(
                        ZLinkBackendRequestResult.OK,
                        0,
                        committedActorRef,
                        address.spotId(),
                        currentActorRef.generation(),
                        0,
                        List.of(Message.from(admissionReplyBytes)));
                })
                .exceptionallyCompose(installError -> {
                    Throwable installCause = unwrap(installError);
                    admissionReply.close();
                    services.actors().failRemoteMove(actor, installCause);
                    return CompletableFuture.failedFuture(installCause);
                });
        });
    }

    private CompletionStage<Void> forwardHandoffPackets(
        SpotTransportAddress address,
        ZLinkBackendActorRef targetActorRef,
        List<ZLinkActorHandoffPacket> packets) {
        CompletionStage<Void> tail = CompletableFuture.completedFuture(null);
        for (int index = 0; index < packets.size(); index++) {
            ZLinkActorHandoffPacket packet = packets.get(index);
            int remainingStart = index + 1;
            tail = tail
                .thenCompose(ignored -> forwardHandoffPacket(
                    address, targetActorRef, packet))
                .whenComplete((ignored, error) -> {
                    if (error != null && remainingStart < packets.size()) {
                        failPackets(
                            packets.subList(remainingStart, packets.size()),
                            unwrap(error));
                    }
                });
        }
        return tail;
    }

    private CompletionStage<Void> forwardHandoffPacket(
        SpotTransportAddress address,
        ZLinkBackendActorRef targetActorRef,
        ZLinkActorHandoffPacket packet) {
        List<Message> parts = ZLinkActorSpotRoutePackets.createActorPacketParts(
            targetActorRef,
            packet.header(),
            packet.payload(),
            packet.replyRoute(),
            packet.arrivalIndex(),
            packet.acceptedJournalRecord());
        CompletionStage<Void> forwarded;
        try {
            forwarded = requestTransfer(address, parts)
                .handle((replyParts, error) -> {
                    try {
                        if (error != null) {
                            if (packet.fail(error)) {
                                packet.close();
                            }
                            throw new CompletionException(error);
                        }
                        if ("1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"))) {
                            java.util.logging.Logger.getLogger(
                                    ZLinkActorSpotJoinCall.class.getName())
                                .warning("[zlink-java-stream-trace] late backlog reply"
                                    + " parts=" + replyParts.size()
                                    + " directAck="
                                    + (!replyParts.isEmpty()
                                        && ZLinkActorSpotRoutePackets
                                            .isHandoffDirectReplyAck(
                                                replyParts.get(0))));
                        }
                        packet.complete(replyParts.isEmpty()
                            || ZLinkActorSpotRoutePackets
                                .isHandoffDirectReplyAck(replyParts.get(0))
                            ? java.util.Optional.empty()
                            : java.util.Optional.of(
                                Message.from(replyParts.get(0))));
                        return null;
                    } finally {
                        if (replyParts != null) {
                            replyParts.forEach(Message::close);
                        }
                        if (error == null) {
                            packet.close();
                        }
                    }
                });
        } catch (RuntimeException error) {
            parts.forEach(Message::close);
            throw error;
        }
        return forwarded.whenComplete((ignored, error) ->
            parts.forEach(Message::close));
    }

    private ZLinkBackendActorJoinResult decodeCommitReply(
        List<Message> replyParts,
        Message admissionReply,
        List<ZLinkActorHandoffPacket> backlog) {
        try {
            if (replyParts.isEmpty()) {
                throw new ZLinkConfigurationException(
                    "remote actor Spot commit reply was empty: " + spotId);
            }
            if (isFrameworkErrorReply(replyParts)) {
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    frameworkErrorReplyMessage(replyParts));
            }
            ZLinkActorSpotRoutePackets.JoinReply reply =
                ZLinkActorSpotRoutePackets.decodeJoinReply(replyParts.get(0));
            if (replyParts.size() != backlog.size() + 1) {
                throw new ZLinkConfigurationException(
                    "remote actor Spot handoff reply count did not match backlog");
            }
            for (int index = 0; index < backlog.size(); index++) {
                ZLinkActorHandoffPacket packet = backlog.get(index);
                Message response = replyParts.get(index + 1);
                packet.complete(response.toByteArray().length == 0
                    ? java.util.Optional.empty()
                    : java.util.Optional.of(Message.from(response)));
                packet.close();
            }
            Message commitReply = reply.reply();
            try (commitReply) {
                return new ZLinkBackendActorJoinResult(
                    ZLinkBackendRequestResult.OK,
                    reply.accepted() ? 0 : 1,
                    reply.actorRef(),
                    spotId,
                    reply.actorRef().generation(),
                    0,
                    List.of(Message.from(admissionReply)));
            }
        } finally {
            admissionReply.close();
            replyParts.forEach(Message::close);
        }
    }

    private record TransferCommit(
        ZLinkActorTransferRegistry.TransferState transfer,
        List<ZLinkActorHandoffPacket> backlog) {
    }

    private static void failPackets(
        List<ZLinkActorHandoffPacket> packets,
        Throwable error) {
        packets.forEach(packet -> {
            if (packet.fail(error)) {
                packet.close();
            }
        });
    }

    private record CommitReply(
        List<Message> parts,
        List<ZLinkActorHandoffPacket> backlog) {
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

    private boolean isFrameworkErrorReply(List<Message> parts) {
        return ZLinkFrameworkErrorReply.isReply(parts);
    }

    private String frameworkErrorReplyMessage(List<Message> parts) {
        return ZLinkFrameworkErrorReply.message(parts);
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
            systems.zlink.framework.spots.ZLinkSpotKind.USER);
    }

    private static CompletionStage<SpotHandle> resolveHandle(
        SpotTransportAddressResolver resolver,
        String spotId) {
        if (!(resolver instanceof systems.zlink.framework.spots.SpotHandleResolver handles)) {
            return CompletableFuture.failedFuture(new ZLinkConfigurationException(
                "SPOT transport resolver does not provide opaque handles"));
        }
        return handles.resolveSpotHandle(spotId).thenApply(handle -> handle.orElseThrow(() ->
            new ZLinkConfigurationException("SPOT handle was not found: " + spotId)));
    }

    @FunctionalInterface
    interface ActorJoinedLocationRenewal {
        CompletionStage<Void> renew(ZLinkActor actor, String spotId);
    }

    record Services(
        ZLinkInternalSpotNode spotNode,
        Function<String, ZLinkSpot<?>> spotResolver,
        SpotTransportAddressResolver remoteAddressResolver,
        ZLinkChannelRuntime routedTransport,
        Function<String, String> actorTypes,
        ZLinkMessageSerializer serializer,
        ZLinkMessageFlowTracer flow,
        ZLinkActorRuntime actors,
        ActorJoinedLocationRenewal locationRenewal) {
    }
}
