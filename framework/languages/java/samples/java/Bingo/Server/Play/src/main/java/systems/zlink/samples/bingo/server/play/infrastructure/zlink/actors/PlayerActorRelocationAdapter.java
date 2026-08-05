package systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

public final class PlayerActorRelocationAdapter
    implements ZLinkActorRelocationAdapter<PlayerActor> {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Override
    public java.util.concurrent.CompletionStage<byte[]> capture(
        PlayerActor actor,
        ZLinkRelocationCancellation cancellation) {
        try {
            return java.util.concurrent.CompletableFuture.completedFuture(
                JSON.writeValueAsBytes(new TransferState(
                    actor.displayName(),
                    actor.roomId(),
                    actor.destroyAfterEntrySpotJoin(),
                    actor.disconnected())));
        } catch (java.io.IOException error) {
            return java.util.concurrent.CompletableFuture.failedFuture(error);
        }
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> restore(
        PlayerActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        try {
            TransferState transferred = JSON.readValue(state, TransferState.class);
            actor.setDisplayName(transferred.displayName());
            if (transferred.roomId() != null && !transferred.roomId().isBlank()) {
                actor.joinRoom(transferred.roomId());
            }
            if (transferred.destroyAfterEntrySpotJoin()) {
                actor.markForDestroyAfterRoomLeave();
            }
            if (transferred.disconnected()) {
                actor.markDisconnected();
            }
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return java.util.concurrent.CompletableFuture.failedFuture(error);
        }
    }

    public record TransferState(
        String displayName,
        String roomId,
        boolean destroyAfterEntrySpotJoin,
        boolean disconnected) {
    }
}
