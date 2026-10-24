package MyGame.zone;

import MyGame.*;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class PlayerTransfer implements ZLinkActorRelocationAdapter<Player> {
    public CompletionStage<byte[]> capture(Player actor, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(
                    Json.MAPPER.writeValueAsBytes(actor.position()));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            actor.apply(Json.MAPPER.readValue(state, EnterZone.class));
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
