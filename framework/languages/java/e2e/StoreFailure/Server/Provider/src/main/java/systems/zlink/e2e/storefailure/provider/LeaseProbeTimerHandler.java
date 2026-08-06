package systems.zlink.e2e.storefailure.provider;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class LeaseProbeTimerHandler {
    public CompletionStage<Void> handle(
        LeaseProbeSpot spot,
        ZLinkTimerTick tick) {
        spot.evidence().record(
            "lease-timer",
            spot.context().spotId() + ":" + tick.deliveryIndex());
        return CompletableFuture.completedFuture(null);
    }
}
