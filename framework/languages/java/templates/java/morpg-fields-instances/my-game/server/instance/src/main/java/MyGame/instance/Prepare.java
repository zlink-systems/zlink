package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

import java.util.concurrent.*;

public final class Prepare {
    @ZLinkSpotRequest
    public CompletionStage<DungeonPrepared> prepare(Dungeon spot, PrepareDungeon request) {
        return CompletableFuture.completedFuture(spot.prepare(request));
    }
}
