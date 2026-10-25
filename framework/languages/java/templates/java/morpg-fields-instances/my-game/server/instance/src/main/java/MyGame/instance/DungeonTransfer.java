package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

public final class DungeonTransfer implements ZLinkSpotRelocationAdapter<Dungeon> {
    public CompletionStage<byte[]> capture(Dungeon spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        return CompletableFuture.completedFuture(spot.capture());
    }

    public CompletionStage<Void> restore(
            Dungeon spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        spot.restore(state);
        return CompletableFuture.completedFuture(null);
    }
}
