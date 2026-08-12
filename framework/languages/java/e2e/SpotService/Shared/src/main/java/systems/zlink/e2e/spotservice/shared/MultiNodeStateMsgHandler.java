package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class MultiNodeStateMsgHandler
    implements ZLinkSpotPacketHandler<MultiNodeSpot, Contracts.MultiNodeStateMsg> {
    @Override
    public CompletionStage<Void> handle(
        MultiNodeSpot spot,
        Contracts.MultiNodeStateMsg message) {
        spot.command(message.marker());
        return CompletableFuture.completedFuture(null);
    }
}
