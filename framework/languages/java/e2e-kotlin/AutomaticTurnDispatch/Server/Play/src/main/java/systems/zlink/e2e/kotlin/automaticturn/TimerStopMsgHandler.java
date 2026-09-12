package systems.zlink.e2e.kotlin.automaticturn;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

public final class TimerStopMsgHandler
    implements ZLinkSpotPacketHandler<ProbeSpot, Contracts.TimerStopMsg> {
    @Override
    public CompletionStage<Void> handle(
        ProbeSpot spot,
        Contracts.TimerStopMsg message) {
        return spot.stopTimers(message.requestId());
    }
}
