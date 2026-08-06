package systems.zlink.e2e.storefailure.provider;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.storefailure.shared.Contracts;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

public final class LeaseProbeRequestHandler
    implements ZLinkSpotRequestHandler<
        LeaseProbeSpot,
        Contracts.InstanceReq,
        Contracts.InstanceRes> {

    @Override
    public CompletionStage<Contracts.InstanceRes> handle(
        LeaseProbeSpot spot,
        Contracts.InstanceReq request) {
        spot.evidence().record(request.marker(), request.spotId());
        return CompletableFuture.completedFuture(new Contracts.InstanceRes(
            spot.context().spotId(),
            spot.evidence().rid(),
            spot.evidence().lifecycleId(),
            spot.context().objectGeneration(),
            request.marker()));
    }
}
