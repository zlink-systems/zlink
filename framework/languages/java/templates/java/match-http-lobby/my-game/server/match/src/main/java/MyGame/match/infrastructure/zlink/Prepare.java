package MyGame.match.infrastructure.zlink;

import MyGame.*;

import systems.zlink.framework.handlers.*;

import java.util.concurrent.*;

@ZLinkHandlerGroup(Names.ROOM)
public final class Prepare {
    @ZLinkSpotRequest
    public CompletionStage<RoomPrepared> prepare(Room room, PrepareRoom request) {
        return CompletableFuture.completedFuture(room.prepare(request));
    }
}
