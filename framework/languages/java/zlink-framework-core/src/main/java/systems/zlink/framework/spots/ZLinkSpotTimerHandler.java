package systems.zlink.framework.spots;

import java.util.concurrent.CompletionStage;

public interface ZLinkSpotTimerHandler<TSpot extends ZLinkSpot<?>> {
    CompletionStage<Void> handle(TSpot spot, ZLinkTimerTick tick);
}
