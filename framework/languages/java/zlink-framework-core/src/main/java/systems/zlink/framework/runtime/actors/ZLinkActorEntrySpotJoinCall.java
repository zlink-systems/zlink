package systems.zlink.framework.runtime.actors;

import java.time.Duration;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorJoinCall;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendRequestResult;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

final class ZLinkActorEntrySpotJoinCall implements ZLinkActorJoinCall {
    private final ZLinkActorRuntime.DefaultActorContext context;
    private final TargetResolver targetResolver;
    private final Message request;
    private final Duration timeout;
    private final Services services;
    private java.util.concurrent.atomic.AtomicBoolean deferred =
        new java.util.concurrent.atomic.AtomicBoolean();

    ZLinkActorEntrySpotJoinCall(
        ZLinkActorRuntime.DefaultActorContext context,
        TargetResolver targetResolver,
        Message request,
        Duration timeout,
        Services services) {
        this.context = context;
        this.targetResolver = targetResolver;
        this.request = request;
        this.timeout = timeout;
        this.services = services;
    }

    @Override
    public ZLinkActorJoinCall timeout(Duration timeout) {
        if (timeout == null || timeout.isNegative() || timeout.isZero()) {
            throw new ZLinkConfigurationException("timeout must be positive");
        }
        ZLinkActorEntrySpotJoinCall configured = new ZLinkActorEntrySpotJoinCall(
            context,
            targetResolver,
            request,
            timeout,
            services);
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
        ZLinkActorSpotJoinCall.validateTimeout(timeout);
        long now = System.nanoTime();
        long timeoutNanos = timeout.toNanos();
        long deadline = timeoutNanos >= Long.MAX_VALUE - now
            ? Long.MAX_VALUE
            : now + timeoutNanos;
        ZLinkActorJoinOperationId operationId =
            ZLinkActorSpotJoinCall.newOperationId();
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
        rejectSameGateWait();
        Message requestPart = Message.from(request);
        try {
            return manage(targetResolver.resolve(timeout)
                .thenCompose(target -> services.spotNode().joinActorEntrySpot(
                    context.actorRef(),
                    target.nodeRid(),
                    requestPart,
                    timeout)
                .thenCompose(result -> {
                    if (result.result() != ZLinkBackendRequestResult.OK) {
                        throw new ZLinkConfigurationException(
                            "actor entry spot join failed: " + result.result());
                    }
                    CompletionStage<Void> route = result.joinResultCode() != 0
                        || result.actor().equals(context.actorRef())
                        ? CompletableFuture.completedFuture(null)
                        : context.rebindNativeActor(result.actor(), timeout);
                    return route.thenCompose(ignored -> {
                        if (result.joinResultCode() == 0) {
                            context.markMovedToEntrySpot(result.actor(), target);
                        }
                        ZLinkActorJoinOutcome decoded = ZLinkActorJoinResults.decode(
                            services.serializer(),
                            result.joinResultCode(),
                            result.actor(),
                            context.meshName(),
                            result.replyParts());
                        return result.joinResultCode() == 0
                            ? services.locationRenewal().renew(
                                context.actor(), result.targetNodeRid())
                                .thenApply(ignoredRoute -> decoded)
                            : CompletableFuture.completedFuture(decoded);
                    });
                })));
        } finally {
            requestPart.close();
        }
    }

    private CompletionStage<Void> executeDeferred(
        ZLinkActorJoinOperationId operationId,
        long deadlineNanos) {
        Duration remaining =
            ZLinkActorSpotJoinCall.remainingTimeout(deadlineNanos);
        if (remaining == null) {
            return notifyCompletion(new ZLinkActorJoinCompletion.Failed(
                operationId,
                ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED));
        }
        ZLinkActorEntrySpotJoinCall bounded =
            (ZLinkActorEntrySpotJoinCall) timeout(remaining);
        // Deferred Entry Spot Join is also an infrastructure/lifecycle
        // operation. Do not carry the application execution context from the
        // handler into its same-Actor wait guard.
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(null)) {
            return bounded.execute().handle((result, error) -> {
                    if (error != null) {
                        Throwable cause = error instanceof java.util.concurrent.CompletionException
                            && error.getCause() != null ? error.getCause() : error;
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
                .thenCompose(this::notifyCompletion);
        }
    }

    private CompletionStage<Void> notifyCompletion(
        ZLinkActorJoinCompletion completion) {
        try {
            CompletionStage<Void> stage =
                context.actor().onJoinCompleted(completion);
            return stage == null
                ? CompletableFuture.completedFuture(null)
                : stage;
        } catch (RuntimeException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    private static <T> CompletionStage<T> manage(CompletionStage<T> stage) {
        return systems.zlink.framework.execution.ZLinkAsyncSerialQueue.manageCurrent(stage);
    }

    private void rejectSameGateWait() {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectCurrentActorJoinWait(
                context.actorRef().actorId());
    }

    @FunctionalInterface
    interface TargetResolver {
        CompletionStage<ZLinkActorRuntime.EntrySpotTarget> resolve(
            Duration timeout);
    }

    @FunctionalInterface
    interface ActorMovedToEntrySpotLocationRenewal {
        CompletionStage<Void> renew(ZLinkActor actor, RoutingId nodeRid);
    }

    record Services(
        ZLinkInternalSpotNode spotNode,
        ZLinkMessageSerializer serializer,
        ZLinkActorRuntime actors,
        ActorMovedToEntrySpotLocationRenewal locationRenewal) {
    }
}
