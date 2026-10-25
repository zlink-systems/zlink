package MyGame.field;

import MyGame.Transfer;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.spots.ZLinkSpotRelocationAdapter;

import java.util.concurrent.*;

public final class PartyTransfer implements ZLinkSpotRelocationAdapter<PartySpot> {
    public CompletionStage<byte[]> capture(PartySpot spot, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        return CompletableFuture.completedFuture(spot.capture());
    }

    public CompletionStage<Void> restore(
            PartySpot spot, byte[] state, ZLinkRelocationCancellation cancellation) {
        Transfer.check(cancellation);
        spot.restore(state);
        return CompletableFuture.completedFuture(null);
    }
}
