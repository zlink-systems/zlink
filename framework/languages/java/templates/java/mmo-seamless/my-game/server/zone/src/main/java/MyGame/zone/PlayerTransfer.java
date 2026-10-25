package MyGame.zone;

import MyGame.Transfer;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerTransfer implements ZLinkActorRelocationAdapter<Player> {
    public CompletionStage<byte[]> capture(Player actor, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        return CompletableFuture.completedFuture(new byte[0]);
    }

    public CompletionStage<Void> restore(
            Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        if (state.length != 0)
            throw new IllegalArgumentException("Player Actor has no application relocation state");
        return CompletableFuture.completedFuture(null);
    }
}
