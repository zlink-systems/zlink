package systems.zlink.samples.bingo.server.play.infrastructure.zlink.actors;
import java.io.IOException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import com.fasterxml.jackson.databind.ObjectMapper;
import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

public final class PlayerActorRelocationAdapter
    implements ZLinkActorRelocationAdapter<PlayerActor> {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Override
    public CompletionStage<byte[]> capture(
        PlayerActor actor,
        ZLinkRelocationCancellation cancellation) {
        try {
            return CompletableFuture.completedFuture(
                JSON.writeValueAsBytes(new TransferState(
                    actor.displayName(),
                    actor.roomId(),
                    actor.destroyAfterEntrySpotJoin(),
                    actor.disconnected())));
        } catch (IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    @Override
    public CompletionStage<Void> restore(
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
            return CompletableFuture.completedFuture(null);
        } catch (IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public record TransferState(
        String displayName,
        String roomId,
        boolean destroyAfterEntrySpotJoin,
        boolean disconnected) {
    }
}
