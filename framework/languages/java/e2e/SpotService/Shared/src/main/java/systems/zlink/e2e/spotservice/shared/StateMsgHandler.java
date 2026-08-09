package systems.zlink.e2e.spotservice.shared;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class StateMsgHandler
    implements ZLinkSpotPacketHandler<UserSpot, Contracts.StateMsg> {
    @Override
    public CompletionStage<Void> handle(
        UserSpot spot,
        Contracts.StateMsg message) {
        spot.command(message.value());
        return CompletableFuture.completedFuture(null);
    }
}
