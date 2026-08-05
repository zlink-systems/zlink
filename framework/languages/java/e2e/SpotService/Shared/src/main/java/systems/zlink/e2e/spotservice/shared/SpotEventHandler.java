package systems.zlink.e2e.spotservice.shared;

import systems.zlink.framework.handlers.ZLinkSpotSubscription;
import systems.zlink.framework.spots.ZLinkSpotSubscriptionHandler;

@ZLinkSpotSubscription(topic = "spot.events")
public final class SpotEventHandler
    implements ZLinkSpotSubscriptionHandler<UserSpot, Contracts.MeshMsg> {
    @Override
    public java.util.concurrent.CompletionStage<Void> handle(
        UserSpot spot,
        Contracts.MeshMsg message) {
        spot.record("SpotMeshMsg", message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
