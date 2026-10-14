package MyGame.match.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.ZLinkMessageContext;
import systems.zlink.framework.handlers.*;
import systems.zlink.framework.spots.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.PLAYER)
public final class Join {
    private final ZLinkSpotManager spots;

    public Join(ZLinkSpotManager spots) {
        this.spots = spots;
    }

    @ZLinkSpotActorSend
    public CompletionStage<Void> join(
            Entry entry, Player actor, ZLinkMessageContext dispatch, JoinRoomRequest request) {
        if (!actor.context().actorId().equals(request.playerId()))
            throw new IllegalArgumentException("Player identity differs from Actor");
        return spots.getOrCreate(request.roomId(), Names.ROOM)
                .inMesh(Names.MESH)
                .request(request)
                .submit()
                .thenAccept(
                        created -> {
                            if (created.state() == ZLinkSpotCreateState.REJECTED)
                                throw new IllegalStateException("Room creation rejected");
                            actor.context().joinSpot(request.roomId(), request).defer();
                        });
    }
}
