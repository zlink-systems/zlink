package MyGame.zone;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class EnterWorld {
    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry entry, Player actor, ZLinkMessageContext dispatch, JoinWorldRequest request) {
        actor.checkIdentity(request.playerId());
        var location = ZonePositionDomain.resolveLocation(request.x());
        if (location.rejection() != null)
            return actor.context().boundSession().send(location.rejection()).submit();
        actor.move(location.zoneId(), new EnterZone(new Position(request.x()), 0, 0));
        return CompletableFuture.completedFuture(null);
    }
}
