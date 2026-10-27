package MyGame.channel;

import MyGame.Json;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerTransfer implements ZLinkActorRelocationAdapter<Player> {
    public CompletionStage<byte[]> capture(Player actor, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    Json.MAPPER.writeValueAsBytes(actor.goldBalance().capture()));
        } catch (java.io.IOException e) {
            return CompletableFuture.failedFuture(e);
        }
    }

    public CompletionStage<Void> restore(
            Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            actor.restore(Json.MAPPER.readValue(state, GoldBalance.Snapshot.class));
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException e) {
            return CompletableFuture.failedFuture(e);
        }
    }
}
