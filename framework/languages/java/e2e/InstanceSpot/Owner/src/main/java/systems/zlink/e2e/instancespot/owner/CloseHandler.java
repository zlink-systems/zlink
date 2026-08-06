package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class CloseHandler implements ZLinkSpotPacketHandler<
    ProbeSpot,
    Contracts.CloseRequest> {
    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.CloseRequest request) {
        return spot.gates().await(request.gateId()).thenCompose(ignored ->
            spot.context().close().thenAccept(closed -> spot.evidence().record(
                "CLOSE_RESULT",
                spot.context().spotId(),
                request.operationId(),
                "",
                spot.context().objectGeneration(),
                0,
                Boolean.toString(closed))));
    }
}
