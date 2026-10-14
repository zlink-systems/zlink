package MyGame.channel;

import MyGame.*;
import java.util.concurrent.*;
import systems.zlink.framework.actors.ZLinkActorClient;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.spots.*;

public final class Zone implements ZLinkSpot<Player> {
  private final ZLinkSpotContext context;
  private final NodeConfig config;
  private final ZLinkActorClient actors;

  public Zone(ZLinkSpotContext context, NodeConfig config, ZLinkActorClient actors) {
    this.context = context;
    this.config = config;
    this.actors = actors;
  }

  public ZLinkSpotContext context() {
    return context;
  }

  public CompletionStage<ZLinkSpotActorJoinResult> onActorJoin(String id, ZLinkMessage request) {
    return CompletableFuture.completedFuture(
        id.equals(request.decode(EnterWorldRequest.class).playerId())
            ? ZLinkSpotActorJoinResult.accept()
            : ZLinkSpotActorJoinResult.reject());
  }

  private PlayerView view(Player actor) {
    return new PlayerView(
        config.worldId(), Names.channel(context.spotId()), config.id(), actor.gold());
  }

  public CompletionStage<Void> show(Player actor) {
    return actor.context().boundSession().send(view(actor)).submit();
  }

  public CompletionStage<Void> onJoinedActor(Player actor) {
    return actors.sendToActor(actor.context().actorId(), view(actor)).submit();
  }

  public CompletionStage<Void> onLeaveActor(Player actor) {
    return CompletableFuture.completedFuture(null);
  }
}
