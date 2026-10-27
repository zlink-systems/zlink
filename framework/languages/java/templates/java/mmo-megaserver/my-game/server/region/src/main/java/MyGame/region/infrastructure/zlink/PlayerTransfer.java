package MyGame.region.infrastructure.zlink;

import MyGame.Transfer;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

// Player has no application state; identity, membership and binding belong to Framework.
public final class PlayerTransfer implements ZLinkActorRelocationAdapter<Player> {
    private static final byte[] EMPTY = new byte[0];

    public CompletionStage<byte[]> capture(Player actor, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        return CompletableFuture.completedFuture(EMPTY);
    }

    public CompletionStage<Void> restore(
            Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        if (state.length != 0) throw new IllegalArgumentException("Unexpected Player state");
        return CompletableFuture.completedFuture(null);
    }
}
