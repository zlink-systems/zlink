package MyGame.zone;

import MyGame.NodeConfig;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerFactory implements ZLinkActorFactory {
    private final NodeConfig config;

    public PlayerFactory(NodeConfig config) {
        this.config = config;
    }

    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new Player(context, config));
    }
}
