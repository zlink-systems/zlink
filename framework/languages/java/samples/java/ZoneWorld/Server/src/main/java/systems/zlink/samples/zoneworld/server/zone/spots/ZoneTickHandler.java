package systems.zlink.samples.zoneworld.server.zone.spots;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.spots.ZLinkSpotTimerHandler;
import systems.zlink.framework.spots.ZLinkTimerTick;

public final class ZoneTickHandler implements ZLinkSpotTimerHandler<ZoneSpot> {
    @Override
    public CompletionStage<Void> handle(ZoneSpot spot, ZLinkTimerTick tick) {
        return spot.tick();
    }
}
