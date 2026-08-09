package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class ZoneMoveHandler {
    @ZLinkSpotActorSend
    public CompletionStage<Void> handle(
        ZoneSpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.MoveMsg message) {
        return spot.move(actor, message.x(), message.y());
    }
}
