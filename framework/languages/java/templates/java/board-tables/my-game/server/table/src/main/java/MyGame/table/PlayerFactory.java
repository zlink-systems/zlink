package MyGame.table;

import systems.zlink.framework.actors.*;
import systems.zlink.framework.channels.ZLinkRouteClient;

import java.util.concurrent.*;

public final class PlayerFactory implements ZLinkActorFactory {
    private final ZLinkRouteClient routes;

    public PlayerFactory(ZLinkRouteClient routes) {
        this.routes = routes;
    }

    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return CompletableFuture.completedFuture(new Player(context, routes));
    }
}
