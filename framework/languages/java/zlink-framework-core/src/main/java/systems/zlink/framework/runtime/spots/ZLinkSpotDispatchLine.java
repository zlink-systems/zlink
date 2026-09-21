package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

interface SpotDispatchLine {
    CompletionStage<Void> enqueueDispatch(Supplier<CompletionStage<Void>> operation);

    /** Returns whether all application sources share this Spot's gate. */
    default boolean usesSharedExecutionGate() {
        return false;
    }

    /** Runtime control work is independent from the application dispatch lane. */
    default CompletionStage<Void> enqueueInfrastructureDispatch(
            Supplier<CompletionStage<Void>> operation) {
        return enqueueDispatch(operation);
    }

    default CompletionStage<Void> enqueueDispatch(
            long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return enqueueDispatch(operation);
    }

    CompletionStage<Void> enqueueActorDispatch(
            String actorId, Supplier<CompletionStage<Void>> operation);

    default CompletionStage<Void> enqueueActorDispatch(
            String actorId, long payloadBytes, Supplier<CompletionStage<Void>> operation) {
        return enqueueActorDispatch(actorId, operation);
    }

    CompletionStage<Void> enqueueActorDispatch(
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease);

    String spotId();

    DefaultSpotOutbound dispatchOutbound();

    ZLinkSpotHandlerCatalog handlerCatalog();

    ZLinkHandlerInstanceOwner handlerInstances();
}
