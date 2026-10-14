package MyGame.region.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Enter {
    @ZLinkSpotActorSend
    public CompletionStage<Void> enter(
            Entry entry, Player actor, ZLinkMessageContext dispatch, EnterRegionRequest request) {
        actor.checkIdentity(request.playerId());
        return entry.select(actor, request.region(), 1);
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> next(
            Entry entry, Player actor, ZLinkMessageContext dispatch, RegionFull reply) {
        if (!Names.REGION_FULL.equals(reply.reason()))
            throw new IllegalArgumentException("Unknown join rejection");
        return entry.select(actor, reply.region(), Math.incrementExact(reply.index()));
    }
}
