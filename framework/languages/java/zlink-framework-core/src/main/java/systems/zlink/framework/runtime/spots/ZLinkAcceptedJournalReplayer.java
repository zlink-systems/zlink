package systems.zlink.framework.runtime.spots;

import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

/**
 * Decodes Framework-owned accepted records and dispatches them through the
 * target runtime before relaying a preserved request reply.
 */
final class ZLinkAcceptedJournalReplayer
    implements ZLinkUserSpotAggregateStagingOwner.JournalReplayer {
    private final SpotDispatch spotDispatch;
    private final ActorDispatch actorDispatch;
    private final ReplyRelay replyRelay;

    ZLinkAcceptedJournalReplayer(
        SpotDispatch spotDispatch,
        ActorDispatch actorDispatch,
        ReplyRelay replyRelay) {
        this.spotDispatch = Objects.requireNonNull(
            spotDispatch,
            "spotDispatch");
        this.actorDispatch = Objects.requireNonNull(
            actorDispatch,
            "actorDispatch");
        this.replyRelay = Objects.requireNonNull(replyRelay, "replyRelay");
    }

    @Override
    public CompletionStage<Void> replay(
        String laneId,
        ZLinkAsyncSerialQueue.QueuedRecord queued) {
        Objects.requireNonNull(laneId, "laneId");
        Objects.requireNonNull(queued, "queued");
        if (laneId.equals("spot") || laneId.startsWith("timer:")) {
            ZLinkSpotAcceptedJournal.Record record =
                ZLinkSpotAcceptedJournal.decode(queued.payload());
            return spotDispatch.dispatch(record).thenCompose(reply ->
                replyRelay.completeSpot(record, queued.sequence(), reply));
        }
        if (laneId.startsWith("actor:")) {
            ZLinkActorAcceptedJournal.Record record =
                ZLinkActorAcceptedJournal.decode(queued.payload());
            if (!laneId.substring("actor:".length()).equals(record.actorId())) {
                return CompletableFuture.failedFuture(
                    new IllegalArgumentException(
                        "accepted Actor journal lane does not match ActorId"));
            }
            return actorDispatch.dispatch(record).thenCompose(reply ->
                replyRelay.completeActor(record, queued.sequence(), reply));
        }
        return CompletableFuture.failedFuture(new IllegalArgumentException(
            "unknown accepted journal lane: " + laneId));
    }

    @FunctionalInterface
    interface SpotDispatch {
        CompletionStage<List<byte[]>> dispatch(
            ZLinkSpotAcceptedJournal.Record record);
    }

    @FunctionalInterface
    interface ActorDispatch {
        CompletionStage<Optional<byte[]>> dispatch(
            ZLinkActorAcceptedJournal.Record record);
    }

    interface ReplyRelay {
        CompletionStage<Void> completeSpot(
            ZLinkSpotAcceptedJournal.Record request,
            long acceptedSequence,
            List<byte[]> reply);

        CompletionStage<Void> completeActor(
            ZLinkActorAcceptedJournal.Record request,
            long acceptedSequence,
            Optional<byte[]> reply);
    }
}
