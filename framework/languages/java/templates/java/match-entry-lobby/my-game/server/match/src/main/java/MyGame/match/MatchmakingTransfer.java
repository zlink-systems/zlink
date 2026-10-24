package MyGame.match;

import MyGame.Json;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class MatchmakingTransfer implements ZLinkSpotRelocationAdapter<Matchmaking> {
    public CompletionStage<byte[]> capture(
            Matchmaking entity, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(entity.state));
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Matchmaking entity, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            entity.state = Json.MAPPER.readValue(state, MatchmakingState.class);
            return CompletableFuture.completedFuture(null);
        } catch (Exception error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
