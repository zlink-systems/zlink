package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class DungeonTransfer implements ZLinkSpotRelocationAdapter<Dungeon> {
    public CompletionStage<byte[]> capture(Dungeon spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            return CompletableFuture.completedFuture(Json.MAPPER.writeValueAsBytes(spot.state));
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }

    public CompletionStage<Void> restore(
            Dungeon spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        try {
            spot.state = Json.MAPPER.readValue(state, DungeonState.class);
            return CompletableFuture.completedFuture(null);
        } catch (java.io.IOException error) {
            return CompletableFuture.failedFuture(error);
        }
    }
}
