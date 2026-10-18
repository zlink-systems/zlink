package systems.zlink.framework.runtime.spots;

import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

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
            return spotDispatch
                    .dispatch(record)
                    .thenCompose(
                            reply -> replyRelay.completeSpot(record, queued.sequence(), reply));
        }
        if (laneId.startsWith("actor:")) {
            ZLinkActorAcceptedJournal.Record record =
                    ZLinkActorAcceptedJournal.decode(queued.payload());
            try {
                requireActorLane(laneId, record);
            } catch (IllegalArgumentException failure) {
                return CompletableFuture.failedFuture(failure);
            }
            return relayActorOutcome(record, queued.sequence(), actorDispatch.dispatch(record));
        }
        return CompletableFuture.failedFuture(
                new IllegalArgumentException("unknown accepted journal lane: " + laneId));
    }

    @Override
    public java.util.function.Supplier<CompletionStage<Void>> prepareReplay(
            String laneId,
            ZLinkSerialExecutionQueue.QueuedRecord queued,
            CompletionStage<Void> terminal,
            java.util.function.Consumer<CompletionStage<Void>> observe) {
        if (!laneId.startsWith("actor:"))
            return ZLinkUserSpotAggregateStagingOwner.JournalReplayer.super.prepareReplay(
                    laneId, queued, terminal, observe);
        var record = ZLinkActorAcceptedJournal.decode(queued.payload());
        requireActorLane(laneId, record);
        var dispatched = new CompletableFuture<Optional<byte[]>>();
        observe.accept(
                terminal.handle(
                                (ignored, failure) -> {
                                    if (failure != null)
                                        return relayActorOutcome(
                                                record,
                                                queued.sequence(),
                                                CompletableFuture.failedFuture(unwrap(failure)));
                                    // Commit completes a transferred, unstarted Entry without
                                    // running its supplier.
                                    // The next FIFO owner also owns its reply; this owner has no
                                    // dispatch outcome.
                                    if (!dispatched.isDone())
                                        return CompletableFuture.<Void>completedFuture(null);
                                    return relayActorOutcome(record, queued.sequence(), dispatched);
                                })
                        .thenCompose(result -> result));
        return () -> {
            var execution = actorDispatch.dispatch(record);
            return execution
                    .whenComplete(
                            (reply, failure) -> {
                                if (failure == null) dispatched.complete(reply);
                                else dispatched.completeExceptionally(unwrap(failure));
                            })
                    .thenApply(reply -> null);
        };
    }

    private static void requireActorLane(String laneId, ZLinkActorAcceptedJournal.Record record) {
        if (!laneId.substring("actor:".length()).equals(record.actorId()))
            throw new IllegalArgumentException(
                    "accepted Actor journal lane does not match ActorId");
    }

    private CompletionStage<Void> relayActorOutcome(
            ZLinkActorAcceptedJournal.Record record,
            long sequence,
            CompletionStage<Optional<byte[]>> dispatched) {
        return dispatched
                .handle(
                        (reply, failure) -> {
                            if (failure == null)
                                return replyRelay.completeActor(record, sequence, reply);
                            Throwable original = unwrap(failure);
                            return replyRelay
                                    .failActor(record, sequence, original)
                                    .thenCompose(
                                            ignored ->
                                                    CompletableFuture.<Void>failedFuture(original));
                        })
                .thenCompose(result -> result);
    }

    private static Throwable unwrap(Throwable failure) {
        while (failure instanceof java.util.concurrent.CompletionException
                && failure.getCause() != null) failure = failure.getCause();
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
                ZLinkSpotAcceptedJournal.Record request, long acceptedSequence, List<byte[]> reply);

        default CompletionStage<Void> failActor(
                ZLinkActorAcceptedJournal.Record request,
                long acceptedSequence,
                Throwable failure) {
            return CompletableFuture.failedFuture(failure);
        }

        CompletionStage<Void> completeActor(
                ZLinkActorAcceptedJournal.Record request,
                long acceptedSequence,
                Optional<byte[]> reply);
    }
}
