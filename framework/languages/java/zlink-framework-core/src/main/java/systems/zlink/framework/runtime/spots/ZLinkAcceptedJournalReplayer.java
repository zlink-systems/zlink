package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Supplier;

/**
 * Decodes Framework-owned accepted records and dispatches them through the target runtime before
 * relaying a preserved request reply.
 */
final class ZLinkAcceptedJournalReplayer
        implements ZLinkUserSpotAggregateStagingOwner.JournalReplayer {
    private final SpotDispatch spotDispatch;
    private final ActorDispatch actorDispatch;
    private final ReplyRelay replyRelay;

    ZLinkAcceptedJournalReplayer(
            SpotDispatch spotDispatch, ActorDispatch actorDispatch, ReplyRelay replyRelay) {
        this.spotDispatch = Objects.requireNonNull(spotDispatch, "spotDispatch");
        this.actorDispatch = Objects.requireNonNull(actorDispatch, "actorDispatch");
        this.replyRelay = Objects.requireNonNull(replyRelay, "replyRelay");
    }

    @Override
    public CompletionStage<Void> replay(
            String laneId, ZLinkSerialExecutionQueue.QueuedRecord queued) {
        Objects.requireNonNull(laneId, "laneId");
        Objects.requireNonNull(queued, "queued");
        if (laneId.equals("spot") || laneId.startsWith("timer:")) {
            ZLinkSpotAcceptedJournal.Record record =
                    ZLinkSpotAcceptedJournal.decode(queued.payload());
            return complete(
                    () -> spotDispatch.dispatch(record),
                    (reply, failure) ->
                            replyRelay.completeSpot(record, queued.sequence(), reply, failure));
        }
        if (laneId.startsWith("actor:")) {
            ZLinkActorAcceptedJournal.Record record =
                    ZLinkActorAcceptedJournal.decode(queued.payload());
            if (!laneId.substring("actor:".length()).equals(record.actorId())) {
                return CompletableFuture.failedFuture(
                        new IllegalArgumentException(
                                "accepted Actor journal lane does not match ActorId"));
            }
            return complete(
                    () -> actorDispatch.dispatch(record),
                    (reply, failure) ->
                            replyRelay.completeActor(record, queued.sequence(), reply, failure));
        }
        return CompletableFuture.failedFuture(
                new IllegalArgumentException("unknown accepted journal lane: " + laneId));
    }

    private static <T> CompletionStage<Void> complete(
            Supplier<CompletionStage<T>> dispatch,
            BiFunction<T, Throwable, CompletionStage<Void>> terminal) {
        return ZLinkHandlerStages.fromStageSupplier(dispatch)
                .handle(
                        (reply, failure) -> {
                            Throwable original = unwrap(failure);
                            return ZLinkHandlerStages.fromStageSupplier(
                                            () -> terminal.apply(reply, original))
                                    .handle(
                                            (ignored, relayFailure) -> {
                                                Throwable relay = unwrap(relayFailure);
                                                if (original != null) {
                                                    if (relay != null && relay != original)
                                                        original.addSuppressed(relay);
                                                    throw new CompletionException(original);
                                                }
                                                if (relay != null)
                                                    throw new CompletionException(relay);
                                                return (Void) null;
                                            });
                        })
                .thenCompose(stage -> stage);
    }

    private static Throwable unwrap(Throwable failure) {
        while (failure instanceof CompletionException && failure.getCause() != null)
            failure = failure.getCause();
        return failure;
    }

    @FunctionalInterface
    interface SpotDispatch {
        CompletionStage<List<byte[]>> dispatch(ZLinkSpotAcceptedJournal.Record record);
    }

    @FunctionalInterface
    interface ActorDispatch {
        CompletionStage<Optional<byte[]>> dispatch(ZLinkActorAcceptedJournal.Record record);
    }

    interface ReplyRelay {
        CompletionStage<Void> completeSpot(
                ZLinkSpotAcceptedJournal.Record request,
                long acceptedSequence,
                List<byte[]> reply,
                Throwable failure);

        CompletionStage<Void> completeActor(
                ZLinkActorAcceptedJournal.Record request,
                long acceptedSequence,
                Optional<byte[]> reply,
                Throwable failure);
    }
}
