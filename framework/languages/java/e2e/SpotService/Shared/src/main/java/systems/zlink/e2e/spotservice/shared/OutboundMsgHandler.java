package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class OutboundMsgHandler
    implements ZLinkSpotPacketHandler<UserSpot, Contracts.OutboundMsg> {
    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        UserSpot spot,
        Contracts.OutboundMsg message) {
        spot.record("SpotToSpotSend", message.value());
        if (message.value().equals("sm-c6-marker")) {
            spot.context().outbound().publish(
                Contracts.ROUTE_CHANNEL,
                "spot.events",
                new Contracts.MeshMsg("publish:sm-c6-marker"))
                .submit();
        }
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
