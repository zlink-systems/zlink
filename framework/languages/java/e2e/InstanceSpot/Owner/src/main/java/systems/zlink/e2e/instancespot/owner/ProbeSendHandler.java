package systems.zlink.e2e.instancespot.owner;

import java.util.concurrent.CompletionStage;
import systems.zlink.e2e.instancespot.shared.Contracts;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class ProbeSendHandler implements ZLinkSpotPacketHandler<
    ProbeSpot,
    Contracts.InstanceMsg> {
    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.InstanceMsg message) {
        return spot.gates().awaitPayload(message.payload()).thenRun(() ->
            spot.evidence().record(
                "SEND_HANDLER",
                spot.context().spotId(),
                message.operationId(),
                message.payload(),
                spot.context().objectGeneration(),
                0,
                "one-way"));
    }
}
