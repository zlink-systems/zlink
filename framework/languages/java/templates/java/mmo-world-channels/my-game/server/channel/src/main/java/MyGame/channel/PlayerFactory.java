package MyGame.channel;

import MyGame.*;

import systems.zlink.framework.actors.*;

import java.util.concurrent.*;

public final class PlayerFactory implements ZLinkActorFactory {
    private final PlayerData data;

    public PlayerFactory(PlayerData data) {
        this.data = data;
    }

    public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
        return data.load(context.actorId())
                .thenApply(
                        gold -> {
                            var actor = new Player(context, data, gold);
                            System.out.println(Control.PLAYER_CREATED + context.actorId());
                            return actor;
                        });
    }
}
