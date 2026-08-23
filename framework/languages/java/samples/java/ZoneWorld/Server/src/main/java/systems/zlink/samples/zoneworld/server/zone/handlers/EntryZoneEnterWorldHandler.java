package systems.zlink.samples.zoneworld.server.zone.handlers;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.spots.ZLinkEntrySpotActorRequestHandler;
import systems.zlink.samples.zoneworld.server.zone.actors.PlayerActor;
import systems.zlink.samples.zoneworld.server.zone.spots.ZoneEntrySpot;
import systems.zlink.samples.zoneworld.shared.Messages;
import systems.zlink.samples.zoneworld.shared.ZoneWorldSpec;

public final class EntryZoneEnterWorldHandler
    implements ZLinkEntrySpotActorRequestHandler<
        ZoneEntrySpot,
        PlayerActor,
        Messages.EnterWorldReq,
        Messages.EnterWorldRes> {
    @Override
    public CompletionStage<Messages.EnterWorldRes> handle(
        ZoneEntrySpot spot,
        PlayerActor actor,
        ZLinkMessageContext context,
        Messages.EnterWorldReq request) {
        if (!ZoneWorldSpec.inRange(request.x(), request.y())) {
            return CompletableFuture.completedFuture(
                new Messages.EnterWorldRes("", request.x(), request.y(), "OutOfRange"));
        }
        actor.prepareEntry(
            request.x(), request.y(), request.isBot(), request.dirX(), request.dirY());
        String zone = ZoneWorldSpec.zoneOf(request.x(), request.y());
        actor.context().joinSpot(zone, new Messages.EnterZoneReq(
                actor.actorId(), request.x(), request.y(), request.isBot(), true, "", false))
            .defer();
        return CompletableFuture.completedFuture(
            new Messages.EnterWorldRes(zone, request.x(), request.y(), null));
    }
}
