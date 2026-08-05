package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class StateMsgHandler
    implements ZLinkSpotPacketHandler<UserSpot, Contracts.StateMsg> {
    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        UserSpot spot,
        Contracts.StateMsg message) {
        spot.command(message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
