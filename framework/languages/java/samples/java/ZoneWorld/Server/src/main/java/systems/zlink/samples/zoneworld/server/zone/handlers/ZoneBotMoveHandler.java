package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.ZLinkHandlerGroup;
import systems.zlink.framework.handlers.ZLinkSpotActorSend;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneSpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldNames;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;
@ZLinkHandlerGroup(ZoneWorldNames.ZONE_CHANNEL)
public final class ZoneBotMoveHandler {
    @ZLinkSpotActorSend
    public CompletionStage<Void> handle(
        ZoneSpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.BotTickMsg message) {
        int targetX = actor.x() + actor.dirX() * ZoneWorldSpec.BOT_STEP;
        int targetY = actor.y() + actor.dirY() * ZoneWorldSpec.BOT_STEP;
        ZoneWorldSpec.MoveDecision decision = ZoneWorldSpec.validateMove(
            actor.x(), actor.y(), targetX, targetY);
        if (!decision.accepted()) {
            actor.reverseDirection();
            return CompletableFuture.completedFuture(null);
        }
        return spot.move(actor, targetX, targetY);
    }
}
