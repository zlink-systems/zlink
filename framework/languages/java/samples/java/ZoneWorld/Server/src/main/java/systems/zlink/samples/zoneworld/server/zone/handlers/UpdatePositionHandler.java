package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class UpdatePositionHandler
    implements ZLinkSpotPacketHandler<ZoneSpot, Messages.UpdatePositionMsg> {
    @Override
    public CompletionStage<Void> handle(ZoneSpot spot, Messages.UpdatePositionMsg message) {
        spot.applyPosition(message);
        return CompletableFuture.completedFuture(null);
    }
}
