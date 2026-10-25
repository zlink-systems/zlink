package MyGame.match.infrastructure.zlink;

import MyGame.Names;
import MyGame.PrepareRoom;
import MyGame.RoomPrepared;

import systems.zlink.framework.handlers.*;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

@ZLinkHandlerGroup(Names.ROOM)
public final class Prepare {
    @ZLinkSpotRequest
    public CompletionStage<RoomPrepared> prepare(Room room, PrepareRoom request) {
        return CompletableFuture.completedFuture(room.prepare(request));
    }
}
