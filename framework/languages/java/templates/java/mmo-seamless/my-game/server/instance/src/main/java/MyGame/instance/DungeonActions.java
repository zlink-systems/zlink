package MyGame.instance;

import MyGame.*;

import systems.zlink.framework.handlers.ZLinkSpotRequest;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

public final class DungeonActions {
    private final NodeConfig config;

    public DungeonActions(NodeConfig config) {
        this.config = config;
    }

    @ZLinkSpotRequest
    public CompletionStage<DungeonOpened> open(Dungeon spot, OpenDungeonRequest request) {
        if (!spot.context().spotId().equals(request.dungeonId()))
            throw new IllegalArgumentException("Dungeon identity mismatch");
        return CompletableFuture.completedFuture(
                new DungeonOpened(spot.context().spotId(), config.id()));
    }
}
