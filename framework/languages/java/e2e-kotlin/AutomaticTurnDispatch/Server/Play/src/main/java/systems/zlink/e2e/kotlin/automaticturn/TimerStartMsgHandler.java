package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class TimerStartMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.TimerStartMsg> {
    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.TimerStartMsg message) {
        spot.startTimer(message);
        return CompletableFuture.completedFuture(null);
    }
}
