package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class TimerStopMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.TimerStopMsg> {
    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.TimerStopMsg message) {
        spot.stopTimers(message.requestId());
        return CompletableFuture.completedFuture(null);
    }
}
