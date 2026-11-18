package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;

interface SpotDispatchLine {
    static SpotDispatchLine from(Object surface) {
        Object context =
                surface instanceof systems.zlink.framework.spots.ZLinkSpot<?> spot
                        ? spot.context()
                        : surface instanceof systems.zlink.framework.spots.ZLinkEntrySpot<?> entry
                                ? entry.context()
                                : null;
        return context instanceof SpotDispatchLine line ? line : null;
    }

    void requireExecution();

    CompletionStage<Void> enqueueLifecycle(Supplier<CompletionStage<Void>> operation);

    long objectGeneration();

    systems.zlink.contracts.core.RoutingId nodeRid();

    default int spotKind() {
        return 2;
    }

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
            String actorId,
            Supplier<byte[]> acceptedJournalRecord,
            long acceptedJournalRecordSizeHint,
            Supplier<CompletionStage<Void>> operation,
            Runnable relocationRelease,
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload);

    CompletionStage<Void> enqueueActorDispatch(
            String actorId,
            long payloadBytes,
            Supplier<CompletionStage<Void>> operation,
            CompletableFuture<Void> admission,
            ZLinkBackendActorReceived header,
            Message payload);

    String spotId();

    DefaultSpotOutbound dispatchOutbound();

    ZLinkSpotHandlerCatalog handlerCatalog();

    ZLinkHandlerInstanceOwner handlerInstances();
}
