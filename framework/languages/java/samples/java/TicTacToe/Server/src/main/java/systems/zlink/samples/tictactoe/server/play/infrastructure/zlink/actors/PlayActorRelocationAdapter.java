package systems.zlink.samples.tictactoe.server.play.infrastructure.zlink.actors;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.samples.tictactoe.shared.contracts.PlayerInfo;

// --8<-- [start:doc-relocation-adapter]
public final class PlayActorRelocationAdapter
    implements ZLinkActorRelocationAdapter<PlayActor> {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Override
    public java.util.concurrent.CompletionStage<byte[]> capture(
        PlayActor actor,
        ZLinkRelocationCancellation cancellation) {
        try {
            return java.util.concurrent.CompletableFuture.completedFuture(
                JSON.writeValueAsBytes(new TransferState(
                    actor.joinedRoomId(),
                    actor.playerOrNull(),
                    actor.destroyAfterEntrySpotJoin(),
                    actor.disconnected())));
        } catch (java.io.IOException error) {
            return java.util.concurrent.CompletableFuture.failedFuture(error);
        }
    }

    @Override
    public java.util.concurrent.CompletionStage<Void> restore(
        PlayActor actor,
        byte[] state,
        ZLinkRelocationCancellation cancellation) {
        try {
            TransferState transferred = JSON.readValue(state, TransferState.class);
            if (transferred.player() != null) {
                actor.applyPlayer(transferred.player());
            }
            if (transferred.roomId() != null && !transferred.roomId().isBlank()) {
                actor.joinGame(transferred.roomId());
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
        String roomId,
        PlayerInfo player,
        boolean destroyAfterEntrySpotJoin,
        boolean disconnected) {
    }
}
// --8<-- [end:doc-relocation-adapter]
