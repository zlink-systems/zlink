package systems.zlink.framework.runtime.spots;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.spots.ZLinkSpot;

abstract class ZLinkSpotContextHost {
    abstract Executor serialExecutor();

    /** Executor for runtime control work that must progress during relocation. */
    abstract Executor infrastructureExecutor();

    abstract DefaultSpotOutbound createContextOutbound(
        ZLinkBackendSpot backendSpot,
        RoutingId nodeRid);

    abstract ZLinkSpotTimerRegistry createTimerRegistry(
        String spotId,
        ZLinkHandlerInstanceOwner handlers,
        ZLinkSpotTimerRegistry.Dispatch dispatch);

    abstract ZLinkHandlerInstanceOwner createHandlerInstances();

    abstract CompletionStage<Void> destroyActorFromEntry(
        RoutingId nodeRid,
        ZLinkActor actor);

    abstract CompletionStage<Void> leaveActor(
        RoutingId nodeRid,
        ZLinkSpot<?> spot,
        ZLinkActor actor,
        String fallbackSpotId);

    abstract CompletionStage<Boolean> closeSpot(String spotId);

    abstract CompletionStage<Boolean> closeInstanceSpot(
        String spotId,
        long objectGeneration);

    abstract CompletionStage<Boolean> completeInstanceSpotClose(
        ZLinkInstanceSpotActivation activation);

    abstract boolean isActorMember(String spotId, String actorId);

    abstract boolean isActorAtSpot(String actorId, String spotId);

    abstract Object deferredActorJoinRuntimeScope();

    abstract <T> CompletionStage<T> runWithOutbound(
        DefaultSpotOutbound outbound,
        Supplier<CompletionStage<T>> operation);

    abstract CompletionStage<Void> runEntryDispatch(
        Object entryContext,
        Supplier<CompletionStage<Void>> operation);

    abstract CompletionStage<Void> runActorTimerDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation);

    abstract CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation);
}
