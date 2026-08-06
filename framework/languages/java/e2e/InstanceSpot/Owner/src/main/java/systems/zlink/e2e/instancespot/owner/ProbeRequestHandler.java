package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.framework.spots.ZLinkSpotRequestHandler;

public final class ProbeRequestHandler implements ZLinkSpotRequestHandler<
    ProbeSpot,
    Contracts.InstanceRequest,
    Contracts.InstanceReply> {
    @Override
    public CompletionStage<Contracts.InstanceReply> handle(
        ProbeSpot spot,
        Contracts.InstanceRequest request) {
        spot.enterHandler(request.operationId(), request.payload());
        return spot.gates().awaitPayload(request.payload()).thenApply(ignored -> {
            long sequence = spot.nextHandlerSequence();
            Contracts.InstanceReply reply = new Contracts.InstanceReply(
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
