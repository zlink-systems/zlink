package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

public final class ProbeRequestHandler implements ZLinkSpotRequestHandler<
    ProbeSpot,
    Contracts.InstanceReq,
    Contracts.InstanceRes> {
    @Override
    public CompletionStage<Contracts.InstanceRes> handle(
        ProbeSpot spot,
        Contracts.InstanceReq request) {
        spot.enterHandler(request.operationId(), request.payload());
        return spot.gates().awaitPayload(request.payload()).thenApply(ignored -> {
            long sequence = spot.nextHandlerSequence();
            Contracts.InstanceRes reply = new Contracts.InstanceRes(
                spot.context().spotId(),
                request.operationId(),
                request.payload(),
                spot.evidence().rid(),
                spot.evidence().lifecycleId(),
                spot.context().objectGeneration(),
                sequence);
            spot.leaveHandler(request.operationId(), request.payload());
            return reply;
        });
    }
}
