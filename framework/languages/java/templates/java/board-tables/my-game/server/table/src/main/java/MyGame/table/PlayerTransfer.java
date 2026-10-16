package MyGame.table;

import systems.zlink.framework.actors.ZLinkActorRelocationAdapter;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class PlayerTransfer implements ZLinkActorRelocationAdapter<Player> {
    public CompletionStage<byte[]> capture(Player actor, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        return CompletableFuture.completedFuture(new byte[0]);
    }

    public CompletionStage<Void> restore(
            Player actor, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        if (state.length != 0)
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("Player has no application state"));
        return CompletableFuture.completedFuture(null);
    }
}
