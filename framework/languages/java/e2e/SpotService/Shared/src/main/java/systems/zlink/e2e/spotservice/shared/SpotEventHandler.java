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
        if (message.value().equals("publish:sm-c6-marker")) {
            String gateKey = "c6-delivery:" + spot.context().spotId();
            if (spot.context().spotId().startsWith("sm-c6-blocked-")) {
                spot.record("SpotBackpressureEntered", message.value());
                return spot.evidenceGate(gateKey).thenRun(() -> {
                    spot.record("SpotMeshMsg", message.value());
                    spot.record("SpotBackpressureResumed", message.value());
                });
            }
        }
        spot.record("SpotMeshMsg", message.value());
        return java.util.concurrent.CompletableFuture.completedFuture(null);
    }
}
