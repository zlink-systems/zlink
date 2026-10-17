package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.function.Supplier;

abstract class ZLinkSpotContextHost {
    void ensureOwnerAdmissionOpen() {}

    abstract Executor serialExecutor();

    /** Executor for runtime control work that must progress during relocation. */
    abstract Executor infrastructureExecutor();

    abstract DefaultSpotOutbound createContextOutbound(
            ZLinkBackendSpot backendSpot, RoutingId nodeRid);

    abstract ZLinkSpotTimerRegistry createTimerRegistry(
            String spotId,
            ZLinkHandlerInstanceOwner handlers,
            ZLinkSpotTimerRegistry.Dispatch dispatch);

    abstract ZLinkHandlerInstanceOwner createHandlerInstances();

    abstract CompletionStage<Void> destroyActorFromEntry(RoutingId nodeRid, ZLinkActor actor);

    abstract CompletionStage<Void> leaveActor(ZLinkActor actor);

    abstract CompletionStage<Boolean> closeSpot(String spotId, long objectGeneration);

    abstract CompletionStage<Boolean> closeInstanceSpot(String spotId, long objectGeneration);

    abstract CompletionStage<Boolean> completeInstanceSpotClose(
            ZLinkInstanceSpotActivation activation);

    abstract boolean isActorMember(String spotId, String actorId);

    abstract boolean isActorAtSpot(String actorId, String spotId);

    abstract Object deferredActorJoinRuntimeScope();

    abstract <T> CompletionStage<T> runWithOutbound(
            DefaultSpotOutbound outbound, Supplier<CompletionStage<T>> operation);

    abstract CompletionStage<Void> runEntryDispatch(
            Object entryContext, Supplier<CompletionStage<Void>> operation);

    abstract CompletionStage<Void> runActorTimerDispatch(
            String actorId, Supplier<CompletionStage<Void>> operation);

    CompletionStage<Void> runActorTimerDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            Supplier<CompletionStage<Void>> operation) {
        return runActorTimerDispatch(actorId, operation);
    }

    abstract CompletionStage<Void> enqueueActorDispatch(
            String actorId, long payloadBytes, Supplier<CompletionStage<Void>> operation);

    abstract CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission);

    abstract CompletionStage<Void> enqueueActorDispatch(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease);

    abstract CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission);

    CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload) {
        return enqueueActorDispatch(target, actorId, payloadBytes, operation, admission);
    }

    CompletionStage<Void> enqueueActorDispatch(
            ZLinkActorDispatchTarget target,
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload) {
        return enqueueActorDispatch(
                target,
                actorId,
                acceptedJournalRecord,
                acceptedJournalRecordSizeHint,
                operation,
                relocationRelease,
                admission);
    }
}
